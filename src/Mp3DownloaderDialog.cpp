#include "Mp3DownloaderDialog.h"
#include "MediaUtils.h"

#include <wx/dirdlg.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/statbox.h>

#include <chrono>
#include <cstdlib>
#include <random>

namespace fs = std::filesystem;

namespace filmrender {

namespace {

constexpr int kThumbWidth = 96;
constexpr int kThumbHeight = 54;

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

void ApplyToolStatusLabel(wxStaticText* label, const ToolStatus& st) {
    if (st.IsFound()) {
        label->SetLabel(Utf8ToWx(st.name + ": OK (" + st.LocationLabel() + ")"));
        label->SetForegroundColour(wxColour(20, 130, 50));
        label->SetToolTip(Utf8ToWx(PathToUtf8(st.resolvedPath)));
    } else {
        label->SetLabel(Utf8ToWx(st.name + ": Missing"));
        label->SetForegroundColour(wxColour(190, 40, 40));
        label->SetToolTip("Not found in tools/ or PATH");
    }
}

wxBitmap MakePlaceholderThumbnail() {
    wxImage img(kThumbWidth, kThumbHeight);
    img.SetRGB(wxRect(0, 0, kThumbWidth, kThumbHeight), 46, 52, 64);
    return wxBitmap(img);
}

#ifdef _WIN32
std::string EscapePowerShellSingleQuoted(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\'') {
            out += "''";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}
#endif

bool DownloadUrlToFile(const std::string& url,
                       const fs::path& dest,
                       ProcessRunner& runner) {
    int exitCode = -1;
    std::string err;
    auto noop = [](const std::string&, bool) {};
#ifdef _WIN32
    if (runner.Run("curl.exe",
                   {"-fsSL", "--connect-timeout", "10", "--max-time", "20", "-o", PathToUtf8(dest), url},
                   noop, exitCode, err)) {
        return true;
    }
    const std::string psCmd =
        "Invoke-WebRequest -UseBasicParsing -TimeoutSec 20 -Uri '" +
        EscapePowerShellSingleQuoted(url) + "' -OutFile '" +
        EscapePowerShellSingleQuoted(PathToUtf8(dest)) + "'";
    return runner.Run("powershell.exe",
                      {"-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", psCmd},
                      noop, exitCode, err);
#else
    if (runner.Run("curl",
                   {"-fsSL", "--connect-timeout", "10", "--max-time", "20", "-o", PathToUtf8(dest), url},
                   noop, exitCode, err)) {
        return true;
    }
    return runner.Run("wget",
                      {"-q", "--timeout=10", "-O", PathToUtf8(dest), url},
                      noop, exitCode, err);
#endif
}

} // namespace

Mp3DownloaderDialog::Mp3DownloaderDialog(wxWindow* parent,
                                         const fs::path& toolsDir,
                                         const fs::path& initialAudioDir,
                                         const std::string& customFfmpegPath)
    : wxDialog(parent, wxID_ANY, "Download MP3s (yt-dlp Queue)",
               wxDefaultPosition, wxSize(940, 740),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      toolsDir_(toolsDir),
      customFfmpegPath_(customFfmpegPath),
      placeholderThumb_(MakePlaceholderThumbnail()),
      pulseTimer_(this, ID_PulseTimer) {
    SetMinSize(wxSize(760, 580));
    CreateControls();

    if (!initialAudioDir.empty()) {
        audioDirCtrl_->ChangeValue(Utf8ToWx(PathToUtf8(initialAudioDir)));
    }

    StartThumbnailWorker();
    RefreshToolStatuses();
    UpdateQueueSummary();
    UpdateControlStates();

    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnBrowseAudioDir, this, ID_BrowseAudioDir);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnRecheckTools, this, ID_RecheckTools);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnDownloadMissingTools, this, ID_DownloadMissingTools);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnUpdateAllTools, this, ID_UpdateAllTools);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnAddUrlToQueue, this, ID_AddUrlToQueue);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnClearCompleted, this, ID_ClearCompleted);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnClearQueue, this, ID_ClearQueue);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnStartMp3Download, this, ID_StartMp3Download);
    Bind(wxEVT_BUTTON, &Mp3DownloaderDialog::OnStopTask, this, ID_StopTask);
    Bind(wxEVT_TIMER, &Mp3DownloaderDialog::OnPulseTimer, this, ID_PulseTimer);
    Bind(wxEVT_THREAD, &Mp3DownloaderDialog::OnWorkerEvent, this);
    Bind(wxEVT_CLOSE_WINDOW, &Mp3DownloaderDialog::OnClose, this);

    CentreOnParent();
}

Mp3DownloaderDialog::~Mp3DownloaderDialog() {
    pulseTimer_.Stop();
    StopThumbnailWorker();
    if (workerThread_.joinable()) {
        cancelRequested_.store(true);
        runner_.Cancel();
        workerThread_.join();
    }
}

fs::path Mp3DownloaderDialog::GetAudioDir() const {
    return Utf8ToPath(WxToUtf8(audioDirCtrl_->GetValue().Trim(true).Trim(false)));
}

void Mp3DownloaderDialog::CreateControls() {
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // --- 1. Tools & Binary Installer ---
    auto* toolsBox = new wxStaticBoxSizer(wxVERTICAL, this, "Required Tools (PATH or tools/)");

    toolsPathLabel_ = new wxStaticText(
        toolsBox->GetStaticBox(), wxID_ANY,
        Utf8ToWx("tools/ folder: " + PathToUtf8(toolsDir_)),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    toolsBox->Add(toolsPathLabel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    auto* statusAndBtns = new wxBoxSizer(wxHORIZONTAL);
    ytDlpStatusLabel_ = new wxStaticText(toolsBox->GetStaticBox(), wxID_ANY, "yt-dlp: ...");
    bunStatusLabel_ = new wxStaticText(toolsBox->GetStaticBox(), wxID_ANY, "bun: ...");
    ffmpegStatusLabel_ = new wxStaticText(toolsBox->GetStaticBox(), wxID_ANY, "ffmpeg: ...");

    statusAndBtns->Add(ytDlpStatusLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    statusAndBtns->Add(bunStatusLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    statusAndBtns->Add(ffmpegStatusLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    recheckBtn_ = new wxButton(toolsBox->GetStaticBox(), ID_RecheckTools, "Re-check");
    downloadMissingBtn_ = new wxButton(toolsBox->GetStaticBox(), ID_DownloadMissingTools,
                                       "Download Missing -> tools/");
    updateAllBtn_ = new wxButton(toolsBox->GetStaticBox(), ID_UpdateAllTools,
                                 "Update All -> tools/");

    statusAndBtns->Add(recheckBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    statusAndBtns->Add(downloadMissingBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    statusAndBtns->Add(updateAllBtn_, 0, wxALIGN_CENTER_VERTICAL);
    toolsBox->Add(statusAndBtns, 0, wxEXPAND | wxALL, 6);

    mainSizer->Add(toolsBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // --- 2. Destination Folder & Add URL ---
    auto* inputBox = new wxStaticBoxSizer(wxVERTICAL, this, "Audio Folder & Add URL / Playlist");
    auto* grid = new wxFlexGridSizer(2, 3, 6, 8);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(inputBox->GetStaticBox(), wxID_ANY, "Audio Folder:"),
              0, wxALIGN_CENTER_VERTICAL);
    audioDirCtrl_ = new wxTextCtrl(inputBox->GetStaticBox(), wxID_ANY);
    audioDirCtrl_->SetToolTip("Destination folder for downloaded .mp3 files");
    browseAudioBtn_ = new wxButton(inputBox->GetStaticBox(), ID_BrowseAudioDir, "Browse...");
    grid->Add(audioDirCtrl_, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(browseAudioBtn_, 0, wxALIGN_CENTER_VERTICAL);

    grid->Add(new wxStaticText(inputBox->GetStaticBox(), wxID_ANY, "URL / Playlist:"),
              0, wxALIGN_CENTER_VERTICAL);
    urlCtrl_ = new wxTextCtrl(inputBox->GetStaticBox(), wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    urlCtrl_->SetToolTip("Paste a YouTube video or playlist URL and click Add URL to Queue");
    urlCtrl_->Bind(wxEVT_TEXT_ENTER, &Mp3DownloaderDialog::OnAddUrlToQueue, this);
    addUrlBtn_ = new wxButton(inputBox->GetStaticBox(), ID_AddUrlToQueue, "Add URL to Queue");
    grid->Add(urlCtrl_, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(addUrlBtn_, 0, wxALIGN_CENTER_VERTICAL);

    inputBox->Add(grid, 0, wxEXPAND | wxALL, 6);

    auto* checksRow = new wxBoxSizer(wxHORIZONTAL);
    playlistCheck_ = new wxCheckBox(inputBox->GetStaticBox(), wxID_ANY,
                                    "Expand playlist (--yes-playlist)");
    noOverwritesCheck_ = new wxCheckBox(inputBox->GetStaticBox(), wxID_ANY,
                                        "Skip existing (--no-overwrites)");
    playlistCheck_->SetValue(true);
    noOverwritesCheck_->SetValue(true);
    checksRow->Add(playlistCheck_, 0, wxRIGHT, 18);
    checksRow->Add(noOverwritesCheck_, 0);
    inputBox->Add(checksRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    mainSizer->Add(inputBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // --- 3. Splitter: Queue (Top) + Log (Bottom) ---
    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxSP_3D | wxSP_LIVE_UPDATE);
    splitter_->SetMinimumPaneSize(120);

    // Top Pane: Download Queue
    auto* queuePanel = new wxPanel(splitter_);
    auto* queueBox = new wxStaticBoxSizer(wxVERTICAL, queuePanel, "MP3 Download Queue");

    auto* queueHeader = new wxBoxSizer(wxHORIZONTAL);
    queueSummaryLabel_ = new wxStaticText(queueBox->GetStaticBox(), wxID_ANY, "0 items in queue");
    clearCompletedBtn_ = new wxButton(queueBox->GetStaticBox(), ID_ClearCompleted, "Clear Completed");
    clearQueueBtn_ = new wxButton(queueBox->GetStaticBox(), ID_ClearQueue, "Clear All");
    queueHeader->Add(queueSummaryLabel_, 1, wxALIGN_CENTER_VERTICAL);
    queueHeader->Add(clearCompletedBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    queueHeader->Add(clearQueueBtn_, 0, wxALIGN_CENTER_VERTICAL);
    queueBox->Add(queueHeader, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    queueScroll_ = new wxScrolledWindow(queueBox->GetStaticBox(), wxID_ANY,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxVSCROLL | wxBORDER_SUNKEN);
    queueScroll_->SetScrollRate(0, 12);
    queueItemsSizer_ = new wxBoxSizer(wxVERTICAL);
    queueScroll_->SetSizer(queueItemsSizer_);
    queueScroll_->Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        queueScroll_->FitInside();
        evt.Skip();
    });

    queueBox->Add(queueScroll_, 1, wxEXPAND | wxALL, 6);
    queuePanel->SetSizer(queueBox);

    // Bottom Pane: Output Log
    auto* logPanel = new wxPanel(splitter_);
    auto* logBox = new wxStaticBoxSizer(wxVERTICAL, logPanel, "Downloader Log");

    auto* progressRow = new wxBoxSizer(wxHORIZONTAL);
    progressGauge_ = new wxGauge(logBox->GetStaticBox(), wxID_ANY, 100,
                                 wxDefaultPosition, wxSize(140, 18), wxGA_HORIZONTAL | wxGA_SMOOTH);
    statusLabel_ = new wxStaticText(logBox->GetStaticBox(), wxID_ANY, "Idle",
                                    wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    progressRow->Add(progressGauge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    progressRow->Add(statusLabel_, 1, wxALIGN_CENTER_VERTICAL);
    logBox->Add(progressRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    logTextCtrl_ = new wxTextCtrl(logBox->GetStaticBox(), wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    wxFont monoFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    logTextCtrl_->SetFont(monoFont);
    logBox->Add(logTextCtrl_, 1, wxEXPAND | wxALL, 6);
    logPanel->SetSizer(logBox);

    splitter_->SplitHorizontally(queuePanel, logPanel, 330);
    splitter_->SetSashGravity(0.72);
    mainSizer->Add(splitter_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // --- 4. Bottom Action Row ---
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    downloadMp3Btn_ = new wxButton(this, ID_StartMp3Download, "Download Queue");
    wxFont boldFont = downloadMp3Btn_->GetFont();
    boldFont.MakeBold();
    downloadMp3Btn_->SetFont(boldFont);

    stopBtn_ = new wxButton(this, ID_StopTask, "Stop");
    closeBtn_ = new wxButton(this, wxID_CLOSE, "Close");
    closeBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    btnRow->Add(downloadMp3Btn_, 0, wxRIGHT, 8);
    btnRow->Add(stopBtn_, 0, wxRIGHT, 8);
    btnRow->AddStretchSpacer(1);
    btnRow->Add(closeBtn_, 0);

    mainSizer->Add(btnRow, 0, wxEXPAND | wxALL, 10);
    SetSizer(mainSizer);
}

void Mp3DownloaderDialog::RefreshToolStatuses() {
    toolsStatus_ = ToolManager::CheckAll(toolsDir_, customFfmpegPath_);
    ApplyToolStatusLabel(ytDlpStatusLabel_, toolsStatus_.ytDlp);
    ApplyToolStatusLabel(bunStatusLabel_, toolsStatus_.bun);
    ApplyToolStatusLabel(ffmpegStatusLabel_, toolsStatus_.ffmpeg);
    Layout();
}

void Mp3DownloaderDialog::UpdateQueueSummary() {
    std::size_t doneCount = 0;
    std::size_t pendingCount = 0;
    for (const auto& item : items_) {
        if (item.state == ItemState::Done || item.state == ItemState::Skipped) {
            ++doneCount;
        } else if (item.state == ItemState::Queued || item.state == ItemState::Failed) {
            ++pendingCount;
        }
    }
    queueSummaryLabel_->SetLabel(wxString::Format(
        "%zu item(s) in queue (%zu pending, %zu completed)",
        items_.size(), pendingCount, doneCount));
}

void Mp3DownloaderDialog::UpdateControlStates() {
    const bool busy = isBusy_.load();
    bool hasPending = false;
    bool hasCompleted = false;
    for (const auto& item : items_) {
        if (item.state == ItemState::Queued || item.state == ItemState::Failed) {
            hasPending = true;
        }
        if (item.state == ItemState::Done || item.state == ItemState::Skipped) {
            hasCompleted = true;
        }
        if (item.widgets.filenameCtrl) {
            item.widgets.filenameCtrl->Enable(
                item.state != ItemState::Downloading &&
                item.state != ItemState::Done &&
                item.state != ItemState::Skipped);
        }
        if (item.widgets.removeBtn) {
            item.widgets.removeBtn->Enable(!busy && item.state != ItemState::Downloading);
        }
    }

    audioDirCtrl_->Enable(!busy);
    browseAudioBtn_->Enable(!busy);
    urlCtrl_->Enable(!busy);
    addUrlBtn_->Enable(!busy);
    playlistCheck_->Enable(!busy);
    noOverwritesCheck_->Enable(!busy);

    recheckBtn_->Enable(!busy);
    downloadMissingBtn_->Enable(!busy && !toolsStatus_.AllAvailable());
    updateAllBtn_->Enable(!busy);

    clearCompletedBtn_->Enable(!busy && hasCompleted);
    clearQueueBtn_->Enable(!busy && !items_.empty());

    downloadMp3Btn_->Enable(!busy && hasPending);
    stopBtn_->Enable(busy);
    closeBtn_->Enable(!busy);
}

void Mp3DownloaderDialog::AppendLog(const wxString& line) {
    if (line.IsEmpty()) {
        return;
    }
    logTextCtrl_->AppendText(line + "\n");
    logTextCtrl_->ShowPosition(logTextCtrl_->GetLastPosition());
}

bool Mp3DownloaderDialog::HasDuplicateEntry(const YtVideoEntry& entry) const {
    for (const auto& existing : items_) {
        if (!entry.id.empty() && existing.entry.id == entry.id) {
            return true;
        }
        if (!entry.webpageUrl.empty() && existing.entry.webpageUrl == entry.webpageUrl) {
            return true;
        }
    }
    return false;
}

Mp3DownloaderDialog::QueueItem* Mp3DownloaderDialog::FindItemByUid(std::uint64_t uid) {
    for (auto& item : items_) {
        if (item.uid == uid) {
            return &item;
        }
    }
    return nullptr;
}

void Mp3DownloaderDialog::AddQueueItemRow(const YtVideoEntry& entry) {
    QueueItem item;
    item.uid = nextUid_++;
    item.entry = entry;
    item.currentFilename = entry.suggestedFilename;

    {
        std::lock_guard<std::mutex> lock(filenamesMutex_);
        liveFilenames_[item.uid] = item.currentFilename;
    }

    auto* card = new wxPanel(queueScroll_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* cardRow = new wxBoxSizer(wxHORIZONTAL);

    auto* thumbBmp = new wxStaticBitmap(card, wxID_ANY, placeholderThumb_,
                                        wxDefaultPosition, wxSize(kThumbWidth, kThumbHeight));
    cardRow->Add(thumbBmp, 0, wxALIGN_CENTER_VERTICAL | wxALL, 6);

    auto* infoCol = new wxBoxSizer(wxVERTICAL);

    auto* titleLbl = new wxStaticText(card, wxID_ANY, Utf8ToWx(entry.title),
                                      wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    wxFont titleFont = titleLbl->GetFont();
    titleFont.MakeBold();
    titleLbl->SetFont(titleFont);
    titleLbl->SetToolTip(Utf8ToWx(entry.title));
    infoCol->Add(titleLbl, 0, wxEXPAND | wxBOTTOM, 2);

    const wxString metaText = Utf8ToWx("Channel: " + entry.channel + "   •   ID: " + entry.id);
    auto* metaLbl = new wxStaticText(card, wxID_ANY, metaText,
                                     wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    metaLbl->SetForegroundColour(wxColour(105, 110, 120));
    infoCol->Add(metaLbl, 0, wxEXPAND | wxBOTTOM, 4);

    auto* nameRow = new wxBoxSizer(wxHORIZONTAL);
    nameRow->Add(new wxStaticText(card, wxID_ANY, "File:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    auto* fnCtrl = new wxTextCtrl(card, wxID_ANY, Utf8ToWx(item.currentFilename));
    fnCtrl->SetToolTip("Editable MP3 output filename (without .mp3 extension)");
    const std::uint64_t uidCopy = item.uid;
    fnCtrl->Bind(wxEVT_TEXT, [this, uidCopy, fnCtrl](wxCommandEvent&) {
        const std::string val = WxToUtf8(fnCtrl->GetValue());
        std::lock_guard<std::mutex> lock(filenamesMutex_);
        liveFilenames_[uidCopy] = val;
    });
    nameRow->Add(fnCtrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    nameRow->Add(new wxStaticText(card, wxID_ANY, ".mp3"), 0, wxALIGN_CENTER_VERTICAL);
    infoCol->Add(nameRow, 0, wxEXPAND | wxBOTTOM, 4);

    auto* progRow = new wxBoxSizer(wxHORIZONTAL);
    auto* gauge = new wxGauge(card, wxID_ANY, 100, wxDefaultPosition, wxSize(160, 14),
                              wxGA_HORIZONTAL | wxGA_SMOOTH);
    auto* statusLbl = new wxStaticText(card, wxID_ANY, "Queued",
                                       wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    progRow->Add(gauge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    progRow->Add(statusLbl, 1, wxALIGN_CENTER_VERTICAL);
    infoCol->Add(progRow, 0, wxEXPAND);

    cardRow->Add(infoCol, 1, wxEXPAND | wxTOP | wxBOTTOM, 4);

    auto* removeBtn = new wxButton(card, wxID_ANY, "Remove", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    removeBtn->Bind(wxEVT_BUTTON, [this, uidCopy](wxCommandEvent&) {
        RemoveQueueItem(uidCopy);
    });
    cardRow->Add(removeBtn, 0, wxALIGN_CENTER_VERTICAL | wxALL, 6);

    card->SetSizer(cardRow);

    item.widgets.cardPanel = card;
    item.widgets.thumbBitmap = thumbBmp;
    item.widgets.titleLabel = titleLbl;
    item.widgets.metaLabel = metaLbl;
    item.widgets.filenameCtrl = fnCtrl;
    item.widgets.progressGauge = gauge;
    item.widgets.statusLabel = statusLbl;
    item.widgets.removeBtn = removeBtn;

    const std::string thumbUrl = entry.thumbnailUrl;
    items_.push_back(std::move(item));

    queueItemsSizer_->Add(card, 0, wxEXPAND | wxALL, 4);
    queueScroll_->Layout();
    queueScroll_->FitInside();

    UpdateQueueSummary();
    UpdateControlStates();

    if (!thumbUrl.empty()) {
        EnqueueThumbnailFetch(uidCopy, thumbUrl);
    }
}

void Mp3DownloaderDialog::RemoveQueueItem(std::uint64_t uid) {
    if (isBusy_.load()) {
        return;
    }
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->uid == uid) {
            if (it->widgets.cardPanel) {
                queueItemsSizer_->Detach(it->widgets.cardPanel);
                it->widgets.cardPanel->Destroy();
            }
            items_.erase(it);
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(filenamesMutex_);
        liveFilenames_.erase(uid);
    }
    queueScroll_->Layout();
    queueScroll_->FitInside();
    UpdateQueueSummary();
    UpdateControlStates();
}

void Mp3DownloaderDialog::EnqueueThumbnailFetch(std::uint64_t uid, const std::string& thumbUrl) {
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        thumbQueue_.emplace_back(uid, thumbUrl);
    }
    thumbCv_.notify_one();
}

void Mp3DownloaderDialog::StartThumbnailWorker() {
    thumbStop_ = false;
    thumbThread_ = std::thread([this]() {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::error_code ec;
        const fs::path tmpBase = fs::temp_directory_path(ec);

        for (;;) {
            std::pair<std::uint64_t, std::string> job;
            {
                std::unique_lock<std::mutex> lock(thumbMutex_);
                thumbCv_.wait(lock, [this]() { return thumbStop_ || !thumbQueue_.empty(); });
                if (thumbStop_) {
                    return;
                }
                job = std::move(thumbQueue_.front());
                thumbQueue_.pop_front();
            }

            const fs::path tmpImg = tmpBase / ("filmrender_thumb_" + std::to_string(gen()) + ".img");
            if (DownloadUrlToFile(job.second, tmpImg, thumbRunner_)) {
                if (thumbStop_) {
                    fs::remove(tmpImg, ec);
                    return;
                }
                ThumbReadyPayload payload;
                payload.uid = job.first;
                payload.tempImagePath = PathToUtf8(tmpImg);

                auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
                evt->SetInt(WorkerThumbReady);
                evt->SetPayload(payload);
                wxQueueEvent(this, evt);
            } else {
                fs::remove(tmpImg, ec);
            }
        }
    });
}

void Mp3DownloaderDialog::StopThumbnailWorker() {
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        thumbStop_ = true;
        thumbQueue_.clear();
    }
    thumbCv_.notify_all();
    thumbRunner_.Cancel();
    if (thumbThread_.joinable()) {
        thumbThread_.join();
    }
}

void Mp3DownloaderDialog::OnBrowseAudioDir(wxCommandEvent& /*evt*/) {
    wxDirDialog dlg(this, "Select MP3 Audio Folder", audioDirCtrl_->GetValue(),
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        audioDirCtrl_->SetValue(dlg.GetPath());
    }
}

void Mp3DownloaderDialog::OnRecheckTools(wxCommandEvent& /*evt*/) {
    RefreshToolStatuses();
    UpdateControlStates();
    AppendLog("Checked tools in PATH and " + Utf8ToWx(PathToUtf8(toolsDir_)));
}

void Mp3DownloaderDialog::OnDownloadMissingTools(wxCommandEvent& /*evt*/) {
    RefreshToolStatuses();
    std::vector<ToolId> missing;
    if (!toolsStatus_.ytDlp.IsFound()) {
        missing.push_back(ToolId::YtDlp);
    }
    if (!toolsStatus_.bun.IsFound()) {
        missing.push_back(ToolId::Bun);
    }
    if (!toolsStatus_.ffmpeg.IsFound()) {
        missing.push_back(ToolId::Ffmpeg);
    }
    if (missing.empty()) {
        wxMessageBox("All 3 tools (yt-dlp, bun, ffmpeg) are already available.",
                     "Tools Check", wxOK | wxICON_INFORMATION, this);
        return;
    }
    StartToolDownload(missing);
}

void Mp3DownloaderDialog::OnUpdateAllTools(wxCommandEvent& /*evt*/) {
    StartToolDownload({ToolId::YtDlp, ToolId::Bun, ToolId::Ffmpeg});
}

void Mp3DownloaderDialog::StartToolDownload(const std::vector<ToolId>& toolsToDownload) {
    if (isBusy_.load() || toolsToDownload.empty()) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    isBusy_.store(true);
    cancelRequested_.store(false);
    fetchingUrl_ = false;
    UpdateControlStates();
    statusLabel_->SetLabel("Downloading tool binaries into tools/...");
    progressGauge_->Pulse();
    pulseTimer_.Start(100);

    const fs::path toolsDir = toolsDir_;
    workerThread_ = std::thread([this, toolsDir, toolsToDownload]() {
        auto sendLog = [this](const std::string& msg) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerLog);
            evt->SetString(Utf8ToWx(msg));
            wxQueueEvent(this, evt);
        };

        sendLog("==================== Tool Installer ====================");
        sendLog("Target folder: " + PathToUtf8(toolsDir));

        bool allOk = true;
        std::string lastErr;
        for (ToolId id : toolsToDownload) {
            if (cancelRequested_.load()) {
                allOk = false;
                lastErr = "Download cancelled.";
                break;
            }
            std::string err;
            if (!ToolManager::DownloadTool(id, toolsDir, runner_, sendLog, err)) {
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
        if (cancelRequested_.load()) {
            doneEvt->SetExtraLong(2L);
            doneEvt->SetString("Tool download cancelled.");
        } else if (allOk) {
            doneEvt->SetExtraLong(1L);
            doneEvt->SetString("All selected tools installed in tools/.");
        } else {
            doneEvt->SetExtraLong(0L);
            doneEvt->SetString(Utf8ToWx(lastErr));
        }
        wxQueueEvent(this, doneEvt);
    });
}

void Mp3DownloaderDialog::OnAddUrlToQueue(wxCommandEvent& /*evt*/) {
    if (isBusy_.load()) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    RefreshToolStatuses();
    if (!toolsStatus_.ytDlp.IsFound()) {
        wxMessageBox("yt-dlp was not found in PATH or tools/.\n"
                     "Click 'Download Missing -> tools/' first.",
                     "Missing yt-dlp", wxOK | wxICON_WARNING, this);
        return;
    }

    const std::string url = WxToUtf8(urlCtrl_->GetValue().Trim(true).Trim(false));
    if (url.empty()) {
        wxMessageBox("Please enter a YouTube video or playlist URL.",
                     "Missing URL", wxOK | wxICON_WARNING, this);
        return;
    }

    std::vector<std::string> args;
    args.push_back("--flat-playlist");
    args.push_back("--dump-json");
    args.push_back("--ignore-errors");
    args.push_back("--no-warnings");
    args.push_back(playlistCheck_->GetValue() ? "--yes-playlist" : "--no-playlist");
    if (toolsStatus_.bun.IsFound()) {
        args.push_back("--js-runtimes");
        args.push_back("bun:" + PathToUtf8(toolsStatus_.bun.resolvedPath));
    }
    args.push_back(url);

    const std::string ytDlpExe = PathToUtf8(toolsStatus_.ytDlp.resolvedPath);

    isBusy_.store(true);
    cancelRequested_.store(false);
    fetchingUrl_ = true;
    fetchAddedCount_ = 0;
    fetchDupCount_ = 0;
    UpdateControlStates();
    statusLabel_->SetLabel("Fetching metadata via yt-dlp...");
    progressGauge_->Pulse();
    pulseTimer_.Start(100);

    AppendLog("Fetching metadata: " + Utf8ToWx(url));

    workerThread_ = std::thread([this, ytDlpExe, args]() {
        int exitCode = -1;
        std::string runErr;

        const bool ok = runner_.Run(
            ytDlpExe,
            args,
            [this](const std::string& line, bool isProgress) {
                if (isProgress || line.empty()) {
                    return;
                }
                if (line.front() == '{') {
                    YtVideoEntry entry;
                    if (ParseYtDlpJsonLine(line, entry)) {
                        auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
                        evt->SetInt(WorkerItemFetched);
                        evt->SetPayload(entry);
                        wxQueueEvent(this, evt);
                        return;
                    }
                }
                auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
                evt->SetInt(WorkerLog);
                evt->SetString(Utf8ToWx(line));
                wxQueueEvent(this, evt);
            },
            exitCode,
            runErr);

        auto* doneEvt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
        doneEvt->SetInt(WorkerFinished);
        if (cancelRequested_.load() || runner_.WasCancelled()) {
            doneEvt->SetExtraLong(2L);
            doneEvt->SetString("Metadata fetch cancelled.");
        } else if (ok) {
            doneEvt->SetExtraLong(1L);
            doneEvt->SetString("Finished loading items from URL.");
        } else {
            doneEvt->SetExtraLong(0L);
            doneEvt->SetString(Utf8ToWx("Failed to fetch URL metadata: " + runErr));
        }
        wxQueueEvent(this, doneEvt);
    });
}

void Mp3DownloaderDialog::OnClearCompleted(wxCommandEvent& /*evt*/) {
    if (isBusy_.load()) {
        return;
    }
    queueScroll_->Freeze();
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->state == ItemState::Done || it->state == ItemState::Skipped) {
            if (it->widgets.cardPanel) {
                queueItemsSizer_->Detach(it->widgets.cardPanel);
                it->widgets.cardPanel->Destroy();
            }
            {
                std::lock_guard<std::mutex> lock(filenamesMutex_);
                liveFilenames_.erase(it->uid);
            }
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
    queueScroll_->Thaw();
    queueScroll_->Layout();
    queueScroll_->FitInside();
    UpdateQueueSummary();
    UpdateControlStates();
}

void Mp3DownloaderDialog::OnClearQueue(wxCommandEvent& /*evt*/) {
    if (isBusy_.load()) {
        return;
    }
    queueScroll_->Freeze();
    for (auto& item : items_) {
        if (item.widgets.cardPanel) {
            queueItemsSizer_->Detach(item.widgets.cardPanel);
            item.widgets.cardPanel->Destroy();
        }
    }
    items_.clear();
    {
        std::lock_guard<std::mutex> lock(filenamesMutex_);
        liveFilenames_.clear();
    }
    queueScroll_->Thaw();
    queueScroll_->Layout();
    queueScroll_->FitInside();
    UpdateQueueSummary();
    UpdateControlStates();
}

void Mp3DownloaderDialog::OnStartMp3Download(wxCommandEvent& /*evt*/) {
    if (isBusy_.load()) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    RefreshToolStatuses();
    if (!toolsStatus_.ytDlp.IsFound()) {
        wxMessageBox("yt-dlp was not found in PATH or tools/.\n"
                     "Click 'Download Missing -> tools/' first.",
                     "Missing yt-dlp", wxOK | wxICON_WARNING, this);
        return;
    }
    if (!toolsStatus_.ffmpeg.IsFound()) {
        wxMessageBox("ffmpeg was not found in PATH or tools/ (needed to extract MP3 audio).\n"
                     "Click 'Download Missing -> tools/' first.",
                     "Missing ffmpeg", wxOK | wxICON_WARNING, this);
        return;
    }

    const fs::path audioDir = GetAudioDir();
    if (audioDir.empty()) {
        wxMessageBox("Please choose a target Audio Folder.",
                     "Missing Audio Folder", wxOK | wxICON_WARNING, this);
        return;
    }

    std::error_code ec;
    fs::create_directories(audioDir, ec);
    if (ec || !fs::is_directory(audioDir, ec)) {
        wxMessageBox("Could not access or create Audio Folder:\n" +
                         Utf8ToWx(PathToUtf8(audioDir)),
                     "Folder Error", wxOK | wxICON_ERROR, this);
        return;
    }

    struct PendingJob {
        std::uint64_t uid;
        std::string url;
        std::string title;
    };
    std::vector<PendingJob> jobs;
    for (const auto& item : items_) {
        if (item.state == ItemState::Queued || item.state == ItemState::Failed) {
            jobs.push_back({item.uid, item.entry.webpageUrl, item.entry.title});
        }
    }

    if (jobs.empty()) {
        wxMessageBox("No queued items to download. Add a URL to the queue first.",
                     "Empty Queue", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const std::string ytDlpExe = PathToUtf8(toolsStatus_.ytDlp.resolvedPath);
    const std::string ffmpegPath = toolsStatus_.ffmpeg.IsFound() ? PathToUtf8(toolsStatus_.ffmpeg.resolvedPath) : "";
    const std::string bunPath = toolsStatus_.bun.IsFound() ? PathToUtf8(toolsStatus_.bun.resolvedPath) : "";
    const bool noOverwrites = noOverwritesCheck_->GetValue();

    isBusy_.store(true);
    cancelRequested_.store(false);
    fetchingUrl_ = false;
    UpdateControlStates();
    statusLabel_->SetLabel(wxString::Format("Downloading 0 / %zu...", jobs.size()));
    progressGauge_->SetValue(0);

    AppendLog("==================== Queue Download ====================");

    workerThread_ = std::thread([this, jobs, audioDir, ytDlpExe, ffmpegPath, bunPath, noOverwrites]() {
        auto sendLog = [this](const std::string& msg) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerLog);
            evt->SetString(Utf8ToWx(msg));
            wxQueueEvent(this, evt);
        };
        auto sendItemState = [this](std::uint64_t uid, ItemState st, double prog, const std::string& text) {
            ItemStatePayload p{uid, st, prog, text};
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerItemState);
            evt->SetPayload(p);
            wxQueueEvent(this, evt);
        };
        auto sendOverallProgress = [this](const std::string& text) {
            auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
            evt->SetInt(WorkerProgress);
            evt->SetString(Utf8ToWx(text));
            wxQueueEvent(this, evt);
        };

        std::size_t successCount = 0;
        std::size_t failCount = 0;

        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (cancelRequested_.load()) {
                break;
            }
            const auto& job = jobs[i];
            sendOverallProgress("Downloading " + std::to_string(i + 1) + " / " +
                                std::to_string(jobs.size()) + ": " + job.title);

            std::string rawStem;
            {
                std::lock_guard<std::mutex> lock(filenamesMutex_);
                auto it = liveFilenames_.find(job.uid);
                rawStem = (it != liveFilenames_.end()) ? it->second : job.title;
            }
            const std::string cleanStem = SanitizeDownloadFilename(rawStem);

            sendItemState(job.uid, ItemState::Downloading, 0.0, "Starting...");
            sendLog("[" + std::to_string(i + 1) + "/" + std::to_string(jobs.size()) +
                    "] " + job.title + " -> " + cleanStem + ".mp3");

            std::vector<std::string> args;
            args.push_back("--newline");
            args.push_back("--no-playlist");
            args.push_back("-x");
            args.push_back("--audio-format");
            args.push_back("mp3");
            args.push_back("--audio-quality");
            args.push_back("0");
            if (noOverwrites) {
                args.push_back("--no-overwrites");
            }
            if (!ffmpegPath.empty()) {
                args.push_back("--ffmpeg-location");
                args.push_back(ffmpegPath);
            }
            if (!bunPath.empty()) {
                args.push_back("--js-runtimes");
                args.push_back("bun:" + bunPath);
            }
            args.push_back("-P");
            args.push_back(PathToUtf8(audioDir));
            args.push_back("-o");
            args.push_back(cleanStem + ".%(ext)s");
            args.push_back(job.url);

            bool sawSkipped = false;
            int exitCode = -1;
            std::string runErr;

            const bool ok = runner_.Run(
                ytDlpExe,
                args,
                [this, &sendLog, &sawSkipped, uid = job.uid](const std::string& line, bool /*isProgress*/) {
                    if (line.empty()) {
                        return;
                    }
                    double pct = 0.0;
                    std::string st;
                    if (ParseYtDlpProgress(line, pct, st)) {
                        if (line.find("has already been downloaded") != std::string::npos) {
                            sawSkipped = true;
                        }
                        ItemStatePayload p{uid, ItemState::Downloading, pct, st};
                        auto* evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
                        evt->SetInt(WorkerItemProgress);
                        evt->SetPayload(p);
                        wxQueueEvent(this, evt);
                    } else {
                        sendLog(line);
                    }
                },
                exitCode,
                runErr);

            if (cancelRequested_.load() || runner_.WasCancelled()) {
                sendItemState(job.uid, ItemState::Queued, 0.0, "Cancelled");
                break;
            }
            if (ok) {
                ++successCount;
                downloadedMp3s_ = true;
                if (sawSkipped) {
                    sendItemState(job.uid, ItemState::Skipped, 100.0, "Already exists");
                } else {
                    sendItemState(job.uid, ItemState::Done, 100.0, "Done");
                }
            } else {
                ++failCount;
                sendItemState(job.uid, ItemState::Failed, 0.0, "Failed");
                sendLog("Error downloading '" + job.title + "': " + runErr);
            }
        }

        auto* doneEvt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
        doneEvt->SetInt(WorkerFinished);
        if (cancelRequested_.load()) {
            doneEvt->SetExtraLong(2L);
            doneEvt->SetString("Queue download cancelled.");
        } else if (failCount > 0 && successCount == 0) {
            doneEvt->SetExtraLong(0L);
            doneEvt->SetString("All queued items failed to download.");
        } else {
            doneEvt->SetExtraLong(1L);
            doneEvt->SetString(wxString::Format(
                "Queue finished: %zu completed, %zu failed.", successCount, failCount));
        }
        wxQueueEvent(this, doneEvt);
    });
}

void Mp3DownloaderDialog::OnStopTask(wxCommandEvent& /*evt*/) {
    if (!isBusy_.load()) {
        return;
    }
    cancelRequested_.store(true);
    stopBtn_->Enable(false);
    statusLabel_->SetLabel("Stopping...");
    AppendLog("Stopping active task...");
    runner_.Cancel();
}

void Mp3DownloaderDialog::OnPulseTimer(wxTimerEvent& /*evt*/) {
    if (isBusy_.load() && fetchingUrl_) {
        progressGauge_->Pulse();
    } else if (isBusy_.load() && !items_.empty()) {
        // Only pulse during tool downloads (not queue downloads)
        bool anyDownloading = false;
        for (const auto& item : items_) {
            if (item.state == ItemState::Downloading) {
                anyDownloading = true;
                break;
            }
        }
        if (!anyDownloading) {
            progressGauge_->Pulse();
        }
    }
}

void Mp3DownloaderDialog::OnWorkerEvent(wxThreadEvent& evt) {
    switch (evt.GetInt()) {
    case WorkerLog:
        AppendLog(evt.GetString());
        break;
    case WorkerProgress: {
        statusLabel_->SetLabel(evt.GetString());
        // Update overall gauge based on completed items when downloading queue
        if (!items_.empty() && !fetchingUrl_) {
            std::size_t finished = 0;
            for (const auto& item : items_) {
                if (item.state == ItemState::Done || item.state == ItemState::Skipped) {
                    ++finished;
                }
            }
            progressGauge_->SetValue(static_cast<int>((finished * 100) / items_.size()));
        }
        break;
    }
    case WorkerToolsChanged:
        RefreshToolStatuses();
        break;
    case WorkerItemFetched: {
        const YtVideoEntry entry = evt.GetPayload<YtVideoEntry>();
        if (HasDuplicateEntry(entry)) {
            ++fetchDupCount_;
        } else {
            ++fetchAddedCount_;
            AddQueueItemRow(entry);
        }
        break;
    }
    case WorkerItemProgress: {
        const auto p = evt.GetPayload<ItemStatePayload>();
        if (QueueItem* item = FindItemByUid(p.uid)) {
            item->progress = p.progress;
            item->statusText = p.statusText;
            if (item->widgets.progressGauge) {
                item->widgets.progressGauge->SetValue(static_cast<int>(p.progress));
            }
            if (item->widgets.statusLabel) {
                item->widgets.statusLabel->SetLabel(Utf8ToWx(p.statusText));
            }
        }
        break;
    }
    case WorkerItemState: {
        const auto p = evt.GetPayload<ItemStatePayload>();
        if (QueueItem* item = FindItemByUid(p.uid)) {
            item->state = p.state;
            item->progress = p.progress;
            item->statusText = p.statusText;
            if (item->widgets.progressGauge) {
                item->widgets.progressGauge->SetValue(static_cast<int>(p.progress));
            }
            if (item->widgets.statusLabel) {
                item->widgets.statusLabel->SetLabel(Utf8ToWx(p.statusText));
                if (p.state == ItemState::Done || p.state == ItemState::Skipped) {
                    item->widgets.statusLabel->SetForegroundColour(wxColour(20, 130, 50));
                } else if (p.state == ItemState::Failed) {
                    item->widgets.statusLabel->SetForegroundColour(wxColour(190, 40, 40));
                } else {
                    item->widgets.statusLabel->SetForegroundColour(wxNullColour);
                }
            }
            UpdateQueueSummary();
            UpdateControlStates();
        }
        break;
    }
    case WorkerThumbReady: {
        const auto payload = evt.GetPayload<ThumbReadyPayload>();
        const fs::path imgPath = Utf8ToPath(payload.tempImagePath);
        if (QueueItem* item = FindItemByUid(payload.uid)) {
            wxLogNull suppressErrors;
            wxImage img;
            if (img.LoadFile(Utf8ToWx(payload.tempImagePath))) {
                img.Rescale(kThumbWidth, kThumbHeight, wxIMAGE_QUALITY_BILINEAR);
                if (item->widgets.thumbBitmap) {
                    item->widgets.thumbBitmap->SetBitmap(wxBitmap(img));
                }
            }
        }
        std::error_code ec;
        fs::remove(imgPath, ec);
        break;
    }
    case WorkerFinished: {
        pulseTimer_.Stop();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        const bool wasFetching = fetchingUrl_;
        fetchingUrl_ = false;
        isBusy_.store(false);
        RefreshToolStatuses();
        UpdateQueueSummary();
        UpdateControlStates();

        const long code = evt.GetExtraLong();
        if (code == 1L) {
            if (wasFetching) {
                progressGauge_->SetValue(0);
                urlCtrl_->Clear();
                const wxString msg = wxString::Format(
                    "Added %zu video(s) to queue (%zu duplicate(s) skipped).",
                    fetchAddedCount_, fetchDupCount_);
                statusLabel_->SetLabel(msg);
                AppendLog(msg);
            } else {
                progressGauge_->SetValue(100);
                statusLabel_->SetLabel(evt.GetString());
                AppendLog(evt.GetString());
            }
        } else if (code == 2L) {
            progressGauge_->SetValue(0);
            statusLabel_->SetLabel("Cancelled");
            AppendLog(evt.GetString());
        } else {
            progressGauge_->SetValue(0);
            statusLabel_->SetLabel("Failed");
            AppendLog("Error: " + evt.GetString());
            wxMessageBox(evt.GetString(), "Error", wxOK | wxICON_ERROR, this);
        }
        break;
    }
    default:
        break;
    }
}

void Mp3DownloaderDialog::OnClose(wxCloseEvent& evt) {
    if (isBusy_.load()) {
        if (evt.CanVeto()) {
            const int answer = wxMessageBox(
                "A download is currently running. Stop it and close?",
                "Confirm Close",
                wxYES_NO | wxICON_WARNING,
                this);
            if (answer != wxYES) {
                evt.Veto();
                return;
            }
        }
        cancelRequested_.store(true);
        runner_.Cancel();
    }

    pulseTimer_.Stop();
    StopThumbnailWorker();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    evt.Skip();
}

} // namespace filmrender
