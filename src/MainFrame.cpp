#include "MainFrame.h"
#include "Mp3DownloaderDialog.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/config.h>
#include <wx/dirdlg.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace filmrender {

namespace {

std::string WxToUtf8(const wxString& s) {
    const wxScopedCharBuffer buf = s.utf8_str();
    if (!buf) {
        return {};
    }
    return std::string(buf.data(), buf.length());
}

wxString Utf8ToWx(const std::string& s) {
    return wxString::FromUTF8(s.data(), s.size());
}

fs::path DetectDefaultNcsDir() {
    std::error_code ec;
    const fs::path exePath = Utf8ToPath(WxToUtf8(wxStandardPaths::Get().GetExecutablePath()));
    const fs::path cwdPath = fs::current_path(ec);

    const std::vector<fs::path> candidates = {
        cwdPath / "ncs",
        cwdPath.parent_path() / "ncs",
        exePath.parent_path() / "ncs",
        exePath.parent_path().parent_path() / "ncs"
    };

    for (const fs::path& cand : candidates) {
        if (!cand.empty() && fs::exists(cand, ec) && fs::is_directory(cand, ec)) {
            fs::path norm = fs::weakly_canonical(cand, ec);
            return ec ? cand : norm;
        }
    }
    return {};
}

class PathDropTarget final : public wxFileDropTarget {
public:
    explicit PathDropTarget(wxTextCtrl* targetCtrl) : targetCtrl_(targetCtrl) {}

    bool OnDropFiles(wxCoord /*x*/, wxCoord /*y*/, const wxArrayString& filenames) override {
        if (filenames.IsEmpty() || targetCtrl_ == nullptr || !targetCtrl_->IsEnabled()) {
            return false;
        }
        const fs::path dropped = Utf8ToPath(WxToUtf8(filenames[0]));
        std::error_code ec;
        if (fs::is_directory(dropped, ec)) {
            targetCtrl_->SetValue(Utf8ToWx(PathToUtf8(dropped)));
            return true;
        }
        if (fs::is_regular_file(dropped, ec)) {
            targetCtrl_->SetValue(Utf8ToWx(PathToUtf8(dropped.parent_path())));
            return true;
        }
        return false;
    }

private:
    wxTextCtrl* targetCtrl_;
};

} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "FilmRender", wxDefaultPosition, wxSize(1240, 780)),
      pulseTimer_(this, ID_PulseTimer) {
    SetMinSize(wxSize(980, 660));

    toolsDir_ = ToolManager::ResolveToolsDir(
        Utf8ToPath(WxToUtf8(wxStandardPaths::Get().GetExecutablePath())));

    CreateControls();
    CreateStatusBar(2);
    SetStatusText("Ready", 0);

    LoadSettings();
    UpdateMultiplierLabel();
    RefreshVideoQueue(false);
    RefreshToolStatuses();
    UpdateControlStates();

    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseVideoDir, this, ID_BrowseVideoDir);
    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseMp3Dir, this, ID_BrowseMp3Dir);
    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseOutputFile, this, ID_BrowseOutputFile);
    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseFfmpeg, this, ID_BrowseFfmpeg);

    Bind(wxEVT_BUTTON, &MainFrame::OnRefreshQueue, this, ID_RefreshQueue);
    Bind(wxEVT_BUTTON, &MainFrame::OnSanitizeNow, this, ID_SanitizeNow);
    Bind(wxEVT_BUTTON, &MainFrame::OnMoveUp, this, ID_MoveUp);
    Bind(wxEVT_BUTTON, &MainFrame::OnMoveDown, this, ID_MoveDown);
    Bind(wxEVT_BUTTON, &MainFrame::OnSortNatural, this, ID_SortNatural);
    Bind(wxEVT_BUTTON, &MainFrame::OnSortAlpha, this, ID_SortAlpha);
    Bind(wxEVT_BUTTON, &MainFrame::OnCheckAll, this, ID_CheckAll);
    Bind(wxEVT_BUTTON, &MainFrame::OnUncheckAll, this, ID_UncheckAll);

    Bind(wxEVT_BUTTON, &MainFrame::OnRender, this, ID_Render);
    Bind(wxEVT_BUTTON, &MainFrame::OnStop, this, ID_Stop);
    Bind(wxEVT_BUTTON, &MainFrame::OnPreviewCommand, this, ID_PreviewCmd);
    Bind(wxEVT_BUTTON, &MainFrame::OnOpenOutputDir, this, ID_OpenOutputDir);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearLog, this, ID_ClearLog);
    Bind(wxEVT_BUTTON, &MainFrame::OnSaveLog, this, ID_SaveLog);
    Bind(wxEVT_BUTTON, &MainFrame::OnCheckTools, this, ID_CheckTools);
    Bind(wxEVT_BUTTON, &MainFrame::OnDownloadMissingTools, this, ID_DownloadTools);
    Bind(wxEVT_BUTTON, &MainFrame::OnOpenMp3Downloader, this, ID_OpenMp3Downloader);

    Bind(wxEVT_TIMER, &MainFrame::OnPulseTimer, this, ID_PulseTimer);
    Bind(wxEVT_THREAD, &MainFrame::OnWorkerEvent, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    Centre();
}

MainFrame::~MainFrame() {
    pulseTimer_.Stop();
    if (workerThread_.joinable()) {
        cancelTools_.store(true);
        engine_.Cancel();
        toolRunner_.Cancel();
        workerThread_.join();
    }
}

void MainFrame::CreateControls() {
    auto* rootPanel = new wxPanel(this);
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Two-column splitter: Left (Paths + Options + Video Queue), Right (Output Log)
    splitter_ = new wxSplitterWindow(rootPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxSP_3D | wxSP_LIVE_UPDATE);
    splitter_->SetMinimumPaneSize(340);

    auto* leftPanel = new wxPanel(splitter_);
    auto* leftSizer = new wxBoxSizer(wxVERTICAL);

    // --- 1. Folders & Paths ---
    auto* pathsBox = new wxStaticBoxSizer(wxVERTICAL, leftPanel, "Folders & Paths");
    auto* pathsGrid = new wxFlexGridSizer(4, 3, 6, 8);
    pathsGrid->AddGrowableCol(1, 1);

    auto addPathRow = [&](const wxString& labelText,
                          wxTextCtrl*& textOut,
                          wxButton*& btnOut,
                          int btnId,
                          const wxString& btnLabel,
                          const wxString& hint) {
        auto* label = new wxStaticText(pathsBox->GetStaticBox(), wxID_ANY, labelText);
        textOut = new wxTextCtrl(pathsBox->GetStaticBox(), wxID_ANY);
        textOut->SetToolTip(hint);
        btnOut = new wxButton(pathsBox->GetStaticBox(), btnId, btnLabel);
        pathsGrid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        pathsGrid->Add(textOut, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        pathsGrid->Add(btnOut, 0, wxALIGN_CENTER_VERTICAL);
    };

    addPathRow("Film Folder:", videoDirCtrl_, browseVideoBtn_, ID_BrowseVideoDir,
               "Browse...", "Select or drop folder containing .mp4 files");
    addPathRow("MP3 Audio Folder:", mp3DirCtrl_, browseMp3Btn_, ID_BrowseMp3Dir,
               "Browse...", "Folder containing .mp3 background tracks");
    addPathRow("Output File:", outputFileCtrl_, browseOutputBtn_, ID_BrowseOutputFile,
               "Save As...", "Relative filename (e.g. final_output.mp4) or full path");
    addPathRow("FFmpeg Binary:", ffmpegCtrl_, browseFfmpegBtn_, ID_BrowseFfmpeg,
               "Browse...", "ffmpeg (on PATH) or full executable path");

    videoDirCtrl_->SetDropTarget(new PathDropTarget(videoDirCtrl_));
    mp3DirCtrl_->SetDropTarget(new PathDropTarget(mp3DirCtrl_));

    videoDirCtrl_->Bind(wxEVT_TEXT, &MainFrame::OnVideoDirTextChanged, this);
    outputFileCtrl_->Bind(wxEVT_TEXT, &MainFrame::OnOutputTextChanged, this);
    ffmpegCtrl_->Bind(wxEVT_TEXT, &MainFrame::OnFfmpegTextChanged, this);

    pathsBox->Add(pathsGrid, 0, wxEXPAND | wxALL, 6);
    leftSizer->Add(pathsBox, 0, wxEXPAND | wxBOTTOM, 8);

    // --- 2. Render Options ---
    auto* optsBox = new wxStaticBoxSizer(wxVERTICAL, leftPanel, "Render Options");

    auto* topOptsRow = new wxBoxSizer(wxHORIZONTAL);
    topOptsRow->Add(new wxStaticText(optsBox->GetStaticBox(), wxID_ANY, "Speed Multiplier:"),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    speedSpin_ = new wxSpinCtrlDouble(optsBox->GetStaticBox(), wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxDefaultSize,
                                      wxSP_ARROW_KEYS, 0.10, 100.00, 3.70, 0.10);
    speedSpin_->SetDigits(2);
    speedSpin_->Bind(wxEVT_SPINCTRLDOUBLE, &MainFrame::OnSpeedChanged, this);
    topOptsRow->Add(speedSpin_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    tsMultLabel_ = new wxStaticText(optsBox->GetStaticBox(), wxID_ANY, "TS Mult: 0.270270");
    topOptsRow->Add(tsMultLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 18);

    topOptsRow->Add(new wxStaticText(optsBox->GetStaticBox(), wxID_ANY, "Log Level:"),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    wxArrayString logLevels;
    logLevels.Add("error");
    logLevels.Add("warning");
    logLevels.Add("info");
    logLevels.Add("verbose");
    logLevelChoice_ = new wxChoice(optsBox->GetStaticBox(), wxID_ANY, wxDefaultPosition,
                                   wxDefaultSize, logLevels);
    logLevelChoice_->SetSelection(1); // "warning"
    topOptsRow->Add(logLevelChoice_, 0, wxALIGN_CENTER_VERTICAL);

    optsBox->Add(topOptsRow, 0, wxEXPAND | wxALL, 6);

    auto* checksGrid = new wxFlexGridSizer(0, 2, 6, 18);
    sanitizeCheck_ = new wxCheckBox(optsBox->GetStaticBox(), wxID_ANY,
                                    "Sanitize filenames (ASCII a-zA-Z0-9)");
    includeAudioCheck_ = new wxCheckBox(optsBox->GetStaticBox(), wxID_ANY,
                                        "Include MP3 audio");
    shuffleAudioCheck_ = new wxCheckBox(optsBox->GetStaticBox(), wxID_ANY,
                                        "Shuffle MP3s");
    loopAudioCheck_ = new wxCheckBox(optsBox->GetStaticBox(), wxID_ANY,
                                      "Loop MP3 until video ends");
    shortestCheck_ = new wxCheckBox(optsBox->GetStaticBox(), wxID_ANY,
                                    "Cut to shortest (-shortest)");
    overwriteCheck_ = new wxCheckBox(optsBox->GetStaticBox(), wxID_ANY,
                                     "Overwrite existing (-y)");

    sanitizeCheck_->SetValue(true);
    includeAudioCheck_->SetValue(true);
    shuffleAudioCheck_->SetValue(true);
    loopAudioCheck_->SetValue(true);
    shortestCheck_->SetValue(true);
    overwriteCheck_->SetValue(true);

    includeAudioCheck_->Bind(wxEVT_CHECKBOX, &MainFrame::OnIncludeAudioToggled, this);

    checksGrid->Add(sanitizeCheck_, 0, wxALIGN_CENTER_VERTICAL);
    checksGrid->Add(includeAudioCheck_, 0, wxALIGN_CENTER_VERTICAL);
    checksGrid->Add(shuffleAudioCheck_, 0, wxALIGN_CENTER_VERTICAL);
    checksGrid->Add(loopAudioCheck_, 0, wxALIGN_CENTER_VERTICAL);
    checksGrid->Add(shortestCheck_, 0, wxALIGN_CENTER_VERTICAL);
    checksGrid->Add(overwriteCheck_, 0, wxALIGN_CENTER_VERTICAL);

    optsBox->Add(checksGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    leftSizer->Add(optsBox, 0, wxEXPAND | wxBOTTOM, 8);

    // --- 2b. Tools Status Box ---
    auto* toolsBox = new wxStaticBoxSizer(wxVERTICAL, leftPanel, "Tools Status (PATH / tools/)");
    auto* toolsStatusRow = new wxBoxSizer(wxHORIZONTAL);
    ffmpegStatusLabel_ = new wxStaticText(toolsBox->GetStaticBox(), wxID_ANY, "ffmpeg: ...");
    ytDlpStatusLabel_ = new wxStaticText(toolsBox->GetStaticBox(), wxID_ANY, "yt-dlp: ...");
    bunStatusLabel_ = new wxStaticText(toolsBox->GetStaticBox(), wxID_ANY, "bun: ...");
    toolsStatusRow->Add(ffmpegStatusLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    toolsStatusRow->Add(ytDlpStatusLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    toolsStatusRow->Add(bunStatusLabel_, 1, wxALIGN_CENTER_VERTICAL);
    toolsBox->Add(toolsStatusRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    auto* toolsBtnRow = new wxBoxSizer(wxHORIZONTAL);
    checkToolsBtn_ = new wxButton(toolsBox->GetStaticBox(), ID_CheckTools, "Check Tools");
    downloadToolsBtn_ = new wxButton(toolsBox->GetStaticBox(), ID_DownloadTools, "Download Missing -> tools/");
    openMp3DlBtn_ = new wxButton(toolsBox->GetStaticBox(), ID_OpenMp3Downloader, "Download MP3s (yt-dlp)...");
    toolsBtnRow->Add(checkToolsBtn_, 0, wxRIGHT, 6);
    toolsBtnRow->Add(downloadToolsBtn_, 0, wxRIGHT, 6);
    toolsBtnRow->Add(openMp3DlBtn_, 0);
    toolsBox->Add(toolsBtnRow, 0, wxEXPAND | wxALL, 6);
    leftSizer->Add(toolsBox, 0, wxEXPAND | wxBOTTOM, 8);

    // --- 3. Video Queue ---
    auto* queueBox = new wxStaticBoxSizer(wxVERTICAL, leftPanel, "Video Queue (Order & Selection)");

    queueSummaryLabel_ = new wxStaticText(queueBox->GetStaticBox(), wxID_ANY,
                                          "0 videos queued");
    queueBox->Add(queueSummaryLabel_, 0, wxLEFT | wxRIGHT | wxTOP, 6);

    auto* queueRow = new wxBoxSizer(wxHORIZONTAL);
    videoListBox_ = new wxCheckListBox(queueBox->GetStaticBox(), wxID_ANY);
    videoListBox_->Bind(wxEVT_CHECKLISTBOX, &MainFrame::OnQueueToggled, this);
    videoListBox_->Bind(wxEVT_LISTBOX, &MainFrame::OnQueueSelectionChanged, this);
    queueRow->Add(videoListBox_, 1, wxEXPAND | wxRIGHT, 6);

    auto* queueBtns = new wxGridSizer(0, 2, 4, 4);
    refreshBtn_ = new wxButton(queueBox->GetStaticBox(), ID_RefreshQueue, "Refresh");
    sanitizeNowBtn_ = new wxButton(queueBox->GetStaticBox(), ID_SanitizeNow, "Sanitize Names");
    moveUpBtn_ = new wxButton(queueBox->GetStaticBox(), ID_MoveUp, "Move Up");
    moveDownBtn_ = new wxButton(queueBox->GetStaticBox(), ID_MoveDown, "Move Down");
    sortNaturalBtn_ = new wxButton(queueBox->GetStaticBox(), ID_SortNatural, "Natural Sort");
    sortAlphaBtn_ = new wxButton(queueBox->GetStaticBox(), ID_SortAlpha, "A-Z Sort");
    checkAllBtn_ = new wxButton(queueBox->GetStaticBox(), ID_CheckAll, "Check All");
    uncheckAllBtn_ = new wxButton(queueBox->GetStaticBox(), ID_UncheckAll, "Uncheck All");

    for (wxButton* btn : {refreshBtn_, sanitizeNowBtn_, moveUpBtn_, moveDownBtn_,
                          sortNaturalBtn_, sortAlphaBtn_, checkAllBtn_, uncheckAllBtn_}) {
        queueBtns->Add(btn, 0, wxEXPAND);
    }
    queueRow->Add(queueBtns, 0, wxALIGN_TOP);
    queueBox->Add(queueRow, 1, wxEXPAND | wxALL, 6);
    leftSizer->Add(queueBox, 1, wxEXPAND);

    leftPanel->SetSizer(leftSizer);

    // --- Right Column: Log & Progress ---
    auto* logPanel = new wxPanel(splitter_);
    auto* logBox = new wxStaticBoxSizer(wxVERTICAL, logPanel, "Output Log");

    auto* progressRow = new wxBoxSizer(wxHORIZONTAL);
    progressGauge_ = new wxGauge(logBox->GetStaticBox(), wxID_ANY, 100,
                                 wxDefaultPosition, wxSize(140, 18), wxGA_HORIZONTAL | wxGA_SMOOTH);
    statsLabel_ = new wxStaticText(logBox->GetStaticBox(), wxID_ANY, "Idle",
                                   wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    progressRow->Add(progressGauge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    progressRow->Add(statsLabel_, 1, wxALIGN_CENTER_VERTICAL);
    logBox->Add(progressRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    logTextCtrl_ = new wxTextCtrl(logBox->GetStaticBox(), wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    wxFont monoFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    logTextCtrl_->SetFont(monoFont);
    logBox->Add(logTextCtrl_, 1, wxEXPAND | wxALL, 6);
    logPanel->SetSizer(logBox);

    splitter_->SplitVertically(leftPanel, logPanel, 620);
    splitter_->SetSashGravity(0.52);
    mainSizer->Add(splitter_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // --- 4. Action Buttons ---
    auto* actionRow = new wxBoxSizer(wxHORIZONTAL);
    renderBtn_ = new wxButton(rootPanel, ID_Render, "Render Video");
    wxFont boldFont = renderBtn_->GetFont();
    boldFont.MakeBold();
    renderBtn_->SetFont(boldFont);

    stopBtn_ = new wxButton(rootPanel, ID_Stop, "Stop");
    previewBtn_ = new wxButton(rootPanel, ID_PreviewCmd, "Preview Command");
    openFolderBtn_ = new wxButton(rootPanel, ID_OpenOutputDir, "Open Output Folder");
    clearLogBtn_ = new wxButton(rootPanel, ID_ClearLog, "Clear Log");
    saveLogBtn_ = new wxButton(rootPanel, ID_SaveLog, "Save Log...");

    actionRow->Add(renderBtn_, 0, wxRIGHT, 8);
    actionRow->Add(stopBtn_, 0, wxRIGHT, 8);
    actionRow->Add(previewBtn_, 0, wxRIGHT, 8);
    actionRow->Add(openFolderBtn_, 0, wxRIGHT, 8);
    actionRow->AddStretchSpacer(1);
    actionRow->Add(clearLogBtn_, 0, wxRIGHT, 8);
    actionRow->Add(saveLogBtn_, 0);

    mainSizer->Add(actionRow, 0, wxEXPAND | wxALL, 10);

    rootPanel->SetSizer(mainSizer);
}

void MainFrame::LoadSettings() {
    wxConfig cfg("FilmRender");

    wxString videoDir;
    wxString mp3Dir;
    wxString outputFile = "final_output.mp4";
    wxString ffmpegExe = "ffmpeg";
    double speed = 3.7;
    bool sanitize = true;
    bool includeAudio = true;
    bool shuffleAudio = true;
    bool loopAudio = true;
    bool shortest = true;
    bool overwrite = true;
    long logLevelIdx = 1;

    cfg.Read("Paths/VideoDir", &videoDir, "");
    cfg.Read("Paths/Mp3Dir", &mp3Dir, "");
    cfg.Read("Paths/OutputFile", &outputFile, "final_output.mp4");
    cfg.Read("Paths/Ffmpeg", &ffmpegExe, "ffmpeg");
    cfg.Read("Options/Speed", &speed, 3.7);
    cfg.Read("Options/Sanitize", &sanitize, true);
    cfg.Read("Options/IncludeAudio", &includeAudio, true);
    cfg.Read("Options/ShuffleAudio", &shuffleAudio, true);
    cfg.Read("Options/LoopAudio", &loopAudio, true);
    cfg.Read("Options/Shortest", &shortest, true);
    cfg.Read("Options/Overwrite", &overwrite, true);
    cfg.Read("Options/LogLevelIdx", &logLevelIdx, 1);

    if (mp3Dir.IsEmpty()) {
        const fs::path detectedNcs = DetectDefaultNcsDir();
        if (!detectedNcs.empty()) {
            mp3Dir = Utf8ToWx(PathToUtf8(detectedNcs));
        }
    }

    videoDirCtrl_->ChangeValue(videoDir);
    mp3DirCtrl_->ChangeValue(mp3Dir);
    outputFileCtrl_->ChangeValue(outputFile);
    ffmpegCtrl_->ChangeValue(ffmpegExe);

    speedSpin_->SetValue(std::clamp(speed, 0.10, 100.00));
    sanitizeCheck_->SetValue(sanitize);
    includeAudioCheck_->SetValue(includeAudio);
    shuffleAudioCheck_->SetValue(shuffleAudio);
    loopAudioCheck_->SetValue(loopAudio);
    shortestCheck_->SetValue(shortest);
    overwriteCheck_->SetValue(overwrite);
    if (logLevelIdx >= 0 && logLevelIdx < static_cast<long>(logLevelChoice_->GetCount())) {
        logLevelChoice_->SetSelection(static_cast<int>(logLevelIdx));
    }
}

void MainFrame::SaveSettings() {
    wxConfig cfg("FilmRender");
    cfg.Write("Paths/VideoDir", videoDirCtrl_->GetValue());
    cfg.Write("Paths/Mp3Dir", mp3DirCtrl_->GetValue());
    cfg.Write("Paths/OutputFile", outputFileCtrl_->GetValue());
    cfg.Write("Paths/Ffmpeg", ffmpegCtrl_->GetValue());
    cfg.Write("Options/Speed", speedSpin_->GetValue());
    cfg.Write("Options/Sanitize", sanitizeCheck_->GetValue());
    cfg.Write("Options/IncludeAudio", includeAudioCheck_->GetValue());
    cfg.Write("Options/ShuffleAudio", shuffleAudioCheck_->GetValue());
    cfg.Write("Options/LoopAudio", loopAudioCheck_->GetValue());
    cfg.Write("Options/Shortest", shortestCheck_->GetValue());
    cfg.Write("Options/Overwrite", overwriteCheck_->GetValue());
    cfg.Write("Options/LogLevelIdx", static_cast<long>(logLevelChoice_->GetSelection()));
    cfg.Flush();
}

void MainFrame::RefreshVideoQueue(bool preserveOrderAndSelection) {
    const fs::path videoDir = Utf8ToPath(WxToUtf8(videoDirCtrl_->GetValue().Trim(true).Trim(false)));
    const std::string outName = WxToUtf8(outputFileCtrl_->GetValue().Trim(true).Trim(false));
    const fs::path outputPath = RenderEngine::ResolveOutputPath(videoDir, outName);

    std::vector<VideoFileItem> fresh = ScanVideoFiles(videoDir, outputPath, true);

    if (preserveOrderAndSelection && !videoItems_.empty()) {
        std::map<std::string, bool> prevSelected;
        std::vector<VideoFileItem> merged;
        for (const auto& oldItem : videoItems_) {
            prevSelected[PathToUtf8(oldItem.path)] = oldItem.selected;
        }
        for (const auto& oldItem : videoItems_) {
            auto it = std::find_if(fresh.begin(), fresh.end(), [&oldItem](const VideoFileItem& f) {
                return PathsEquivalent(f.path, oldItem.path);
            });
            if (it != fresh.end()) {
                it->selected = oldItem.selected;
                merged.push_back(*it);
                fresh.erase(it);
            }
        }
        for (auto& remaining : fresh) {
            merged.push_back(std::move(remaining));
        }
        videoItems_ = std::move(merged);
    } else {
        videoItems_ = std::move(fresh);
    }

    RebuildQueueDisplay(-1);
}

void MainFrame::RebuildQueueDisplay(int selectIndex) {
    videoListBox_->Freeze();
    videoListBox_->Clear();

    for (std::size_t i = 0; i < videoItems_.size(); ++i) {
        const auto& item = videoItems_[i];
        const wxString label = wxString::Format(
            "%02zu. %s  [%s]",
            i + 1,
            Utf8ToWx(item.filename),
            Utf8ToWx(FormatFileSize(item.sizeBytes)));
        const int idx = videoListBox_->Append(label);
        videoListBox_->Check(static_cast<unsigned int>(idx), item.selected);
    }

    if (selectIndex >= 0 && selectIndex < static_cast<int>(videoItems_.size())) {
        videoListBox_->SetSelection(selectIndex);
    }
    videoListBox_->Thaw();

    UpdateQueueSummary();
    UpdateControlStates();
}

void MainFrame::UpdateQueueSummary() {
    std::size_t selectedCount = 0;
    std::uintmax_t totalBytes = 0;
    for (const auto& item : videoItems_) {
        if (item.selected) {
            ++selectedCount;
            totalBytes += item.sizeBytes;
        }
    }
    queueSummaryLabel_->SetLabel(wxString::Format(
        "%zu selected / %zu total (%s)",
        selectedCount,
        videoItems_.size(),
        Utf8ToWx(FormatFileSize(totalBytes))));
}

void MainFrame::UpdateMultiplierLabel() {
    const std::string mult = ComputeTimestampMultiplier(speedSpin_->GetValue());
    tsMultLabel_->SetLabel("TS Mult: " + Utf8ToWx(mult));
}

void MainFrame::RefreshToolStatuses() {
    const std::string customFfmpeg = WxToUtf8(ffmpegCtrl_->GetValue().Trim(true).Trim(false));
    toolsStatus_ = ToolManager::CheckAll(toolsDir_, customFfmpeg);

    auto applyLabel = [](wxStaticText* label, const ToolStatus& st) {
        if (st.IsFound()) {
            label->SetLabel(Utf8ToWx(st.name + ": OK (" + st.LocationLabel() + ")"));
            label->SetForegroundColour(wxColour(20, 130, 50));
            label->SetToolTip(Utf8ToWx(PathToUtf8(st.resolvedPath)));
        } else {
            label->SetLabel(Utf8ToWx(st.name + ": Missing"));
            label->SetForegroundColour(wxColour(190, 40, 40));
            label->SetToolTip("Not found in PATH or tools/");
        }
    };

    applyLabel(ffmpegStatusLabel_, toolsStatus_.ffmpeg);
    applyLabel(ytDlpStatusLabel_, toolsStatus_.ytDlp);
    applyLabel(bunStatusLabel_, toolsStatus_.bun);
    Layout();
}

void MainFrame::UpdateControlStates() {
    const bool busy = isRendering_.load();
    const bool audioOn = includeAudioCheck_->GetValue();
    const int sel = videoListBox_->GetSelection();
    const int count = static_cast<int>(videoItems_.size());

    videoDirCtrl_->Enable(!busy);
    browseVideoBtn_->Enable(!busy);
    mp3DirCtrl_->Enable(!busy && audioOn);
    browseMp3Btn_->Enable(!busy && audioOn);
    outputFileCtrl_->Enable(!busy);
    browseOutputBtn_->Enable(!busy);
    ffmpegCtrl_->Enable(!busy);
    browseFfmpegBtn_->Enable(!busy);

    speedSpin_->Enable(!busy);
    logLevelChoice_->Enable(!busy);
    sanitizeCheck_->Enable(!busy);
    includeAudioCheck_->Enable(!busy);
    shuffleAudioCheck_->Enable(!busy && audioOn);
    loopAudioCheck_->Enable(!busy && audioOn);
    shortestCheck_->Enable(!busy && audioOn);
    overwriteCheck_->Enable(!busy);

    checkToolsBtn_->Enable(!busy);
    downloadToolsBtn_->Enable(!busy);
    openMp3DlBtn_->Enable(!busy);

    videoListBox_->Enable(!busy);
    refreshBtn_->Enable(!busy);
    sanitizeNowBtn_->Enable(!busy && count > 0);
    moveUpBtn_->Enable(!busy && sel > 0);
    moveDownBtn_->Enable(!busy && sel >= 0 && sel + 1 < count);
    sortNaturalBtn_->Enable(!busy && count > 1);
    sortAlphaBtn_->Enable(!busy && count > 1);
    checkAllBtn_->Enable(!busy && count > 0);
    uncheckAllBtn_->Enable(!busy && count > 0);

    renderBtn_->Enable(!busy);
    stopBtn_->Enable(busy);
    previewBtn_->Enable(!busy);
    clearLogBtn_->Enable(!busy);
}

RenderOptions MainFrame::GatherOptions() const {
    RenderOptions opts;
    opts.videoDir = Utf8ToPath(WxToUtf8(videoDirCtrl_->GetValue().Trim(true).Trim(false)));
    opts.mp3Dir = Utf8ToPath(WxToUtf8(mp3DirCtrl_->GetValue().Trim(true).Trim(false)));
    opts.outputFilenameOrPath = WxToUtf8(outputFileCtrl_->GetValue().Trim(true).Trim(false));
    opts.ffmpegExecutable = WxToUtf8(ffmpegCtrl_->GetValue().Trim(true).Trim(false));
    if ((opts.ffmpegExecutable.empty() || opts.ffmpegExecutable == "ffmpeg" ||
         opts.ffmpegExecutable == "ffmpeg.exe") &&
        toolsStatus_.ffmpeg.IsFound() &&
        toolsStatus_.ffmpeg.location == ToolLocation::ToolsDir) {
        opts.ffmpegExecutable = PathToUtf8(toolsStatus_.ffmpeg.resolvedPath);
    }
    opts.speedFactor = speedSpin_->GetValue();
    opts.sanitizeFilenames = sanitizeCheck_->GetValue();
    opts.includeAudio = includeAudioCheck_->GetValue();
    opts.shuffleAudio = shuffleAudioCheck_->GetValue();
    opts.loopAudio = loopAudioCheck_->GetValue();
    opts.shortest = shortestCheck_->GetValue();
    opts.overwriteOutput = overwriteCheck_->GetValue();
    opts.logLevel = WxToUtf8(logLevelChoice_->GetStringSelection());

    for (const auto& item : videoItems_) {
        if (item.selected) {
            opts.orderedVideos.push_back(item.path);
        }
    }
    return opts;
}

void MainFrame::AppendLog(const wxString& line) {
    if (line.IsEmpty()) {
        return;
    }
    logTextCtrl_->AppendText(line + "\n");
    logTextCtrl_->ShowPosition(logTextCtrl_->GetLastPosition());
}

void MainFrame::OnBrowseVideoDir(wxCommandEvent& /*evt*/) {
    wxDirDialog dlg(this, "Select Film Folder", videoDirCtrl_->GetValue(),
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        videoDirCtrl_->SetValue(dlg.GetPath());
    }
}

void MainFrame::OnBrowseMp3Dir(wxCommandEvent& /*evt*/) {
    wxDirDialog dlg(this, "Select MP3 Audio Folder", mp3DirCtrl_->GetValue(),
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        mp3DirCtrl_->SetValue(dlg.GetPath());
    }
}

void MainFrame::OnBrowseOutputFile(wxCommandEvent& /*evt*/) {
    wxString defaultDir = videoDirCtrl_->GetValue();
    wxString defaultName = outputFileCtrl_->GetValue();
    if (defaultName.IsEmpty()) {
        defaultName = "final_output.mp4";
    }
    wxFileDialog dlg(this, "Choose Output MP4 File", defaultDir, defaultName,
                     "MP4 files (*.mp4)|*.mp4|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() == wxID_OK) {
        outputFileCtrl_->SetValue(dlg.GetPath());
    }
}

void MainFrame::OnBrowseFfmpeg(wxCommandEvent& /*evt*/) {
    wxFileDialog dlg(this, "Select FFmpeg Executable", wxEmptyString, wxEmptyString,
                     "Executable files (*)|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        ffmpegCtrl_->SetValue(dlg.GetPath());
    }
}

void MainFrame::OnVideoDirTextChanged(wxCommandEvent& /*evt*/) {
    RefreshVideoQueue(false);
}

void MainFrame::OnOutputTextChanged(wxCommandEvent& /*evt*/) {
    RefreshVideoQueue(true);
}

void MainFrame::OnFfmpegTextChanged(wxCommandEvent& /*evt*/) {
    RefreshToolStatuses();
}

void MainFrame::OnCheckTools(wxCommandEvent& /*evt*/) {
    RefreshToolStatuses();
    AppendLog("==================== Tools Check ====================");
    AppendLog(Utf8ToWx("tools/ folder: " + PathToUtf8(toolsDir_)));
    auto logOne = [this](const ToolStatus& st) {
        if (st.IsFound()) {
            AppendLog(Utf8ToWx("  " + st.name + ": OK [" + st.LocationLabel() + "] -> " +
                               PathToUtf8(st.resolvedPath)));
        } else {
            AppendLog(Utf8ToWx("  " + st.name + ": MISSING"));
        }
    };
    logOne(toolsStatus_.ffmpeg);
    logOne(toolsStatus_.ytDlp);
    logOne(toolsStatus_.bun);
    SetStatusText("Tools checked", 0);
}

void MainFrame::OnDownloadMissingTools(wxCommandEvent& /*evt*/) {
    if (isRendering_.load()) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    RefreshToolStatuses();
    std::vector<ToolId> toDownload;
    if (!toolsStatus_.ffmpeg.IsFound()) {
        toDownload.push_back(ToolId::Ffmpeg);
    }
    if (!toolsStatus_.ytDlp.IsFound()) {
        toDownload.push_back(ToolId::YtDlp);
    }
    if (!toolsStatus_.bun.IsFound()) {
        toDownload.push_back(ToolId::Bun);
    }

    if (toDownload.empty()) {
        const int ans = wxMessageBox(
            "All 3 tools (ffmpeg, yt-dlp, bun) are already visible.\n"
            "Download latest binaries into tools/ anyway?",
            "Tools Status",
            wxYES_NO | wxICON_QUESTION,
            this);
        if (ans != wxYES) {
            return;
        }
        toDownload = {ToolId::Ffmpeg, ToolId::YtDlp, ToolId::Bun};
    }

    isRendering_.store(true);
    cancelTools_.store(false);
    UpdateControlStates();
    statsLabel_->SetLabel("Downloading tools into tools/...");
    SetStatusText("Downloading tools...", 0);
    progressGauge_->Pulse();
    pulseTimer_.Start(100);

    AppendLog("==================== Tool Installer ====================");
    AppendLog(Utf8ToWx("Target directory: " + PathToUtf8(toolsDir_)));

    const fs::path toolsDir = toolsDir_;
    workerThread_ = std::thread([this, toolsDir, toDownload]() {
        auto sendLog = [this](const std::string& msg) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerLog);
            evt->SetString(Utf8ToWx(msg));
            wxQueueEvent(this, evt);
        };

        bool allOk = true;
        std::string lastErr;
        for (ToolId id : toDownload) {
            if (cancelTools_.load()) {
                allOk = false;
                lastErr = "Tool download cancelled.";
                break;
            }
            std::string err;
            if (!ToolManager::DownloadTool(id, toolsDir, toolRunner_, sendLog, err)) {
                allOk = false;
                lastErr = err;
                sendLog("Error: " + err);
                break;
            }
            auto* changedEvt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            changedEvt->SetInt(WorkerToolsChanged);
            wxQueueEvent(this, changedEvt);
        }

        auto* doneEvt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
        doneEvt->SetInt(WorkerFinished);
        if (cancelTools_.load()) {
            doneEvt->SetExtraLong(2L);
            doneEvt->SetString("Tool download cancelled.");
        } else if (allOk) {
            doneEvt->SetExtraLong(1L);
            doneEvt->SetString("Tools installed into tools/.");
        } else {
            doneEvt->SetExtraLong(0L);
            doneEvt->SetString(Utf8ToWx(lastErr));
        }
        wxQueueEvent(this, doneEvt);
    });
}

void MainFrame::OnOpenMp3Downloader(wxCommandEvent& /*evt*/) {
    const fs::path currentAudioDir = Utf8ToPath(WxToUtf8(mp3DirCtrl_->GetValue().Trim(true).Trim(false)));
    const std::string customFfmpeg = WxToUtf8(ffmpegCtrl_->GetValue().Trim(true).Trim(false));

    Mp3DownloaderDialog dlg(this, toolsDir_, currentAudioDir, customFfmpeg);
    dlg.ShowModal();

    const fs::path chosenDir = dlg.GetAudioDir();
    if (!chosenDir.empty() && !PathsEquivalent(chosenDir, currentAudioDir)) {
        mp3DirCtrl_->ChangeValue(Utf8ToWx(PathToUtf8(chosenDir)));
    }

    RefreshToolStatuses();
    UpdateControlStates();
    if (dlg.DidDownloadMp3s()) {
        const fs::path targetDir = chosenDir.empty() ? currentAudioDir : chosenDir;
        const auto mp3s = ScanAudioFiles(targetDir, false);
        AppendLog(wxString::Format("MP3 folder now contains %zu .mp3 file(s).", mp3s.size()));
    }
}

void MainFrame::OnSpeedChanged(wxSpinDoubleEvent& /*evt*/) {
    UpdateMultiplierLabel();
}

void MainFrame::OnIncludeAudioToggled(wxCommandEvent& /*evt*/) {
    UpdateControlStates();
}

void MainFrame::OnRefreshQueue(wxCommandEvent& /*evt*/) {
    RefreshVideoQueue(true);
    SetStatusText("Video queue refreshed", 0);
}

void MainFrame::OnSanitizeNow(wxCommandEvent& /*evt*/) {
    const fs::path videoDir = Utf8ToPath(WxToUtf8(videoDirCtrl_->GetValue().Trim(true).Trim(false)));
    const std::string outName = WxToUtf8(outputFileCtrl_->GetValue().Trim(true).Trim(false));
    const fs::path outputPath = RenderEngine::ResolveOutputPath(videoDir, outName);

    std::error_code ec;
    if (videoDir.empty() || !fs::exists(videoDir, ec) || !fs::is_directory(videoDir, ec)) {
        wxMessageBox("Please select a valid Film Folder first.", "FilmRender",
                     wxOK | wxICON_WARNING, this);
        return;
    }

    const int answer = wxMessageBox(
        "This will rename .mp4 files in the Film Folder to ASCII alphanumeric names (a-z, A-Z, 0-9).\n\nContinue?",
        "Sanitize Video Filenames",
        wxYES_NO | wxICON_QUESTION,
        this);
    if (answer != wxYES) {
        return;
    }

    AppendLog("==================== Filename Sanitizer ====================");
    std::string err;
    auto renamed = SanitizeVideoFiles(
        videoDir,
        outputPath,
        [this](const std::string& msg) { AppendLog(Utf8ToWx(msg)); },
        &err);

    if (!err.empty()) {
        AppendLog("Error: " + Utf8ToWx(err));
        wxMessageBox(Utf8ToWx(err), "Sanitize Error", wxOK | wxICON_ERROR, this);
    } else if (renamed.empty()) {
        AppendLog("All video filenames are already sanitized.");
    }

    // Update existing queue paths in-place to keep custom order & selection.
    for (auto& item : videoItems_) {
        std::error_code normEc;
        fs::path norm = fs::weakly_canonical(item.path, normEc);
        const std::string key = PathToUtf8(normEc ? item.path : norm);
        auto it = renamed.find(key);
        if (it != renamed.end()) {
            item.path = it->second;
            item.filename = PathToUtf8(it->second.filename());
        }
    }
    RefreshVideoQueue(true);
}

void MainFrame::OnMoveUp(wxCommandEvent& /*evt*/) {
    const int sel = videoListBox_->GetSelection();
    if (sel <= 0 || sel >= static_cast<int>(videoItems_.size())) {
        return;
    }
    std::swap(videoItems_[static_cast<std::size_t>(sel)],
              videoItems_[static_cast<std::size_t>(sel - 1)]);
    RebuildQueueDisplay(sel - 1);
}

void MainFrame::OnMoveDown(wxCommandEvent& /*evt*/) {
    const int sel = videoListBox_->GetSelection();
    if (sel < 0 || sel + 1 >= static_cast<int>(videoItems_.size())) {
        return;
    }
    std::swap(videoItems_[static_cast<std::size_t>(sel)],
              videoItems_[static_cast<std::size_t>(sel + 1)]);
    RebuildQueueDisplay(sel + 1);
}

void MainFrame::OnSortNatural(wxCommandEvent& /*evt*/) {
    std::sort(videoItems_.begin(), videoItems_.end(),
              [](const VideoFileItem& a, const VideoFileItem& b) {
                  return NaturalLess(a.filename, b.filename);
              });
    RebuildQueueDisplay(-1);
}

void MainFrame::OnSortAlpha(wxCommandEvent& /*evt*/) {
    std::sort(videoItems_.begin(), videoItems_.end(),
              [](const VideoFileItem& a, const VideoFileItem& b) {
                  return a.filename < b.filename;
              });
    RebuildQueueDisplay(-1);
}

void MainFrame::OnCheckAll(wxCommandEvent& /*evt*/) {
    for (auto& item : videoItems_) {
        item.selected = true;
    }
    RebuildQueueDisplay(videoListBox_->GetSelection());
}

void MainFrame::OnUncheckAll(wxCommandEvent& /*evt*/) {
    for (auto& item : videoItems_) {
        item.selected = false;
    }
    RebuildQueueDisplay(videoListBox_->GetSelection());
}

void MainFrame::OnQueueToggled(wxCommandEvent& evt) {
    const int idx = evt.GetInt();
    if (idx >= 0 && idx < static_cast<int>(videoItems_.size())) {
        videoItems_[static_cast<std::size_t>(idx)].selected =
            videoListBox_->IsChecked(static_cast<unsigned int>(idx));
        UpdateQueueSummary();
    }
}

void MainFrame::OnQueueSelectionChanged(wxCommandEvent& /*evt*/) {
    UpdateControlStates();
}

void MainFrame::OnRender(wxCommandEvent& /*evt*/) {
    if (isRendering_.load()) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    const RenderOptions opts = GatherOptions();

    std::error_code ec;
    if (opts.videoDir.empty() || !fs::exists(opts.videoDir, ec) || !fs::is_directory(opts.videoDir, ec)) {
        wxMessageBox("Please select a valid Film Folder.", "Validation Error",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    if (opts.orderedVideos.empty()) {
        wxMessageBox("No videos are selected in the queue.", "Validation Error",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    if (opts.includeAudio &&
        (opts.mp3Dir.empty() || !fs::exists(opts.mp3Dir, ec) || !fs::is_directory(opts.mp3Dir, ec))) {
        wxMessageBox("Please select a valid MP3 Audio Folder (or uncheck 'Include MP3 audio').",
                     "Validation Error", wxOK | wxICON_ERROR, this);
        return;
    }

    SaveSettings();

    isRendering_.store(true);
    UpdateControlStates();
    statsLabel_->SetLabel("Starting render...");
    SetStatusText("Rendering...", 0);
    progressGauge_->Pulse();
    pulseTimer_.Start(100);

    AppendLog("============================================================");
    AppendLog("Starting render job...");

    workerThread_ = std::thread([this, opts]() {
        RenderCallbacks cbs;
        cbs.onLog = [this](const std::string& line) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerLog);
            evt->SetString(Utf8ToWx(line));
            wxQueueEvent(this, evt);
        };
        cbs.onStats = [this](const std::string& stats) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerStats);
            evt->SetString(Utf8ToWx(stats));
            wxQueueEvent(this, evt);
        };
        cbs.onPhase = [this](const std::string& phase) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerPhase);
            evt->SetString(Utf8ToWx(phase));
            wxQueueEvent(this, evt);
        };
        cbs.onVideosRenamed = [this](const std::map<std::string, fs::path>& /*renamed*/) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerRenamed);
            wxQueueEvent(this, evt);
        };

        const RenderResult res = engine_.Run(opts, cbs);

        auto* doneEvt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
        doneEvt->SetInt(WorkerFinished);
        doneEvt->SetExtraLong(res.success ? 1L : (res.cancelled ? 2L : 0L));
        doneEvt->SetString(Utf8ToWx(res.message));
        wxQueueEvent(this, doneEvt);
    });
}

void MainFrame::OnStop(wxCommandEvent& /*evt*/) {
    if (!isRendering_.load()) {
        return;
    }
    stopBtn_->Enable(false);
    SetStatusText("Cancelling...", 0);
    AppendLog("Cancellation requested. Stopping active task...");
    cancelTools_.store(true);
    engine_.Cancel();
    toolRunner_.Cancel();
}

void MainFrame::OnPreviewCommand(wxCommandEvent& /*evt*/) {
    const RenderOptions opts = GatherOptions();
    AppendLog(Utf8ToWx(RenderEngine::BuildCommandPreview(opts)));
}

void MainFrame::OnOpenOutputDir(wxCommandEvent& /*evt*/) {
    const fs::path videoDir = Utf8ToPath(WxToUtf8(videoDirCtrl_->GetValue().Trim(true).Trim(false)));
    const std::string outName = WxToUtf8(outputFileCtrl_->GetValue().Trim(true).Trim(false));
    const fs::path outPath = RenderEngine::ResolveOutputPath(videoDir, outName);

    std::error_code ec;
    fs::path dirToOpen = outPath.parent_path();
    if (dirToOpen.empty() || !fs::exists(dirToOpen, ec)) {
        dirToOpen = videoDir;
    }
    if (dirToOpen.empty() || !fs::exists(dirToOpen, ec)) {
        wxMessageBox("Output folder does not exist yet.", "Open Folder",
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxLaunchDefaultApplication(Utf8ToWx(PathToUtf8(dirToOpen)));
}

void MainFrame::OnClearLog(wxCommandEvent& /*evt*/) {
    logTextCtrl_->Clear();
    statsLabel_->SetLabel("Idle");
    progressGauge_->SetValue(0);
    SetStatusText("Ready", 0);
}

void MainFrame::OnSaveLog(wxCommandEvent& /*evt*/) {
    wxFileDialog dlg(this, "Save Log Output", wxEmptyString, "filmrender_log.txt",
                     "Text files (*.txt)|*.txt|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) {
        return;
    }
    const std::string pathStr = WxToUtf8(dlg.GetPath());
    std::ofstream ofs(Utf8ToPath(pathStr), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        wxMessageBox("Failed to save log file.", "Save Error", wxOK | wxICON_ERROR, this);
        return;
    }
    const std::string content = WxToUtf8(logTextCtrl_->GetValue());
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    SetStatusText("Log saved", 0);
}

void MainFrame::OnPulseTimer(wxTimerEvent& /*evt*/) {
    if (isRendering_.load()) {
        progressGauge_->Pulse();
    }
}

void MainFrame::OnWorkerEvent(wxThreadEvent& evt) {
    switch (evt.GetInt()) {
    case WorkerLog:
        AppendLog(evt.GetString());
        break;
    case WorkerStats:
        statsLabel_->SetLabel(evt.GetString());
        break;
    case WorkerPhase:
        SetStatusText(evt.GetString(), 0);
        break;
    case WorkerRenamed:
        RefreshVideoQueue(false);
        break;
    case WorkerToolsChanged:
        RefreshToolStatuses();
        break;
    case WorkerFinished: {
        pulseTimer_.Stop();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        isRendering_.store(false);
        RefreshToolStatuses();
        UpdateControlStates();

        const long code = evt.GetExtraLong();
        if (code == 1L) {
            progressGauge_->SetValue(100);
            statsLabel_->SetLabel("Completed");
            SetStatusText("Completed", 0);
        } else if (code == 2L) {
            progressGauge_->SetValue(0);
            statsLabel_->SetLabel("Cancelled");
            SetStatusText("Cancelled", 0);
        } else {
            progressGauge_->SetValue(0);
            statsLabel_->SetLabel("Failed");
            SetStatusText("Failed", 0);
            wxMessageBox(evt.GetString(), "Task Failed", wxOK | wxICON_ERROR, this);
        }
        break;
    }
    default:
        break;
    }
}

void MainFrame::OnClose(wxCloseEvent& evt) {
    if (isRendering_.load()) {
        if (evt.CanVeto()) {
            const int answer = wxMessageBox(
                "A render is currently in progress. Stop rendering and exit?",
                "Confirm Exit",
                wxYES_NO | wxICON_WARNING,
                this);
            if (answer != wxYES) {
                evt.Veto();
                return;
            }
        }
        cancelTools_.store(true);
        engine_.Cancel();
        toolRunner_.Cancel();
    }

    pulseTimer_.Stop();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    SaveSettings();
    evt.Skip();
}

} // namespace filmrender
