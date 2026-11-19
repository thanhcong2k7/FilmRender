#pragma once

#include "MediaUtils.h"
#include "ProcessRunner.h"
#include "RenderEngine.h"
#include "ToolManager.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <atomic>
#include <thread>
#include <vector>

namespace filmrender {

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    enum {
        ID_BrowseVideoDir = wxID_HIGHEST + 1,
        ID_BrowseMp3Dir,
        ID_BrowseOutputFile,
        ID_BrowseFfmpeg,
        ID_RefreshQueue,
        ID_SanitizeNow,
        ID_MoveUp,
        ID_MoveDown,
        ID_SortNatural,
        ID_SortAlpha,
        ID_CheckAll,
        ID_UncheckAll,
        ID_Render,
        ID_Stop,
        ID_PreviewCmd,
        ID_OpenOutputDir,
        ID_ClearLog,
        ID_SaveLog,
        ID_CheckTools,
        ID_DownloadTools,
        ID_OpenMp3Downloader,
        ID_PulseTimer
    };

    enum WorkerEventKind {
        WorkerLog = 1,
        WorkerStats = 2,
        WorkerPhase = 3,
        WorkerRenamed = 4,
        WorkerFinished = 5,
        WorkerToolsChanged = 6
    };

    void CreateControls();
    void LoadSettings();
    void SaveSettings();

    void RefreshVideoQueue(bool preserveOrderAndSelection = false);
    void RefreshToolStatuses();
    void RebuildQueueDisplay(int selectIndex = -1);
    void UpdateQueueSummary();
    void UpdateMultiplierLabel();
    void UpdateControlStates();

    RenderOptions GatherOptions() const;
    void AppendLog(const wxString& line);

    // Event handlers
    void OnBrowseVideoDir(wxCommandEvent& evt);
    void OnBrowseMp3Dir(wxCommandEvent& evt);
    void OnBrowseOutputFile(wxCommandEvent& evt);
    void OnBrowseFfmpeg(wxCommandEvent& evt);
    void OnVideoDirTextChanged(wxCommandEvent& evt);
    void OnOutputTextChanged(wxCommandEvent& evt);
    void OnFfmpegTextChanged(wxCommandEvent& evt);
    void OnSpeedChanged(wxSpinDoubleEvent& evt);
    void OnIncludeAudioToggled(wxCommandEvent& evt);

    void OnCheckTools(wxCommandEvent& evt);
    void OnDownloadMissingTools(wxCommandEvent& evt);
    void OnOpenMp3Downloader(wxCommandEvent& evt);

    void OnRefreshQueue(wxCommandEvent& evt);
    void OnSanitizeNow(wxCommandEvent& evt);
    void OnMoveUp(wxCommandEvent& evt);
    void OnMoveDown(wxCommandEvent& evt);
    void OnSortNatural(wxCommandEvent& evt);
    void OnSortAlpha(wxCommandEvent& evt);
    void OnCheckAll(wxCommandEvent& evt);
    void OnUncheckAll(wxCommandEvent& evt);
    void OnQueueToggled(wxCommandEvent& evt);
    void OnQueueSelectionChanged(wxCommandEvent& evt);

    void OnRender(wxCommandEvent& evt);
    void OnStop(wxCommandEvent& evt);
    void OnPreviewCommand(wxCommandEvent& evt);
    void OnOpenOutputDir(wxCommandEvent& evt);
    void OnClearLog(wxCommandEvent& evt);
    void OnSaveLog(wxCommandEvent& evt);

    void OnPulseTimer(wxTimerEvent& evt);
    void OnWorkerEvent(wxThreadEvent& evt);
    void OnClose(wxCloseEvent& evt);

    // Path controls
    wxTextCtrl* videoDirCtrl_ = nullptr;
    wxTextCtrl* mp3DirCtrl_ = nullptr;
    wxTextCtrl* outputFileCtrl_ = nullptr;
    wxTextCtrl* ffmpegCtrl_ = nullptr;
    wxButton* browseVideoBtn_ = nullptr;
    wxButton* browseMp3Btn_ = nullptr;
    wxButton* browseOutputBtn_ = nullptr;
    wxButton* browseFfmpegBtn_ = nullptr;

    // Options controls
    wxSpinCtrlDouble* speedSpin_ = nullptr;
    wxStaticText* tsMultLabel_ = nullptr;
    wxChoice* logLevelChoice_ = nullptr;
    wxCheckBox* sanitizeCheck_ = nullptr;
    wxCheckBox* includeAudioCheck_ = nullptr;
    wxCheckBox* shuffleAudioCheck_ = nullptr;
    wxCheckBox* loopAudioCheck_ = nullptr;
    wxCheckBox* shortestCheck_ = nullptr;
    wxCheckBox* overwriteCheck_ = nullptr;

    // Tools status controls
    wxStaticText* ffmpegStatusLabel_ = nullptr;
    wxStaticText* ytDlpStatusLabel_ = nullptr;
    wxStaticText* bunStatusLabel_ = nullptr;
    wxButton* checkToolsBtn_ = nullptr;
    wxButton* downloadToolsBtn_ = nullptr;
    wxButton* openMp3DlBtn_ = nullptr;

    // Queue controls
    wxStaticText* queueSummaryLabel_ = nullptr;
    wxCheckListBox* videoListBox_ = nullptr;
    wxButton* refreshBtn_ = nullptr;
    wxButton* sanitizeNowBtn_ = nullptr;
    wxButton* moveUpBtn_ = nullptr;
    wxButton* moveDownBtn_ = nullptr;
    wxButton* sortNaturalBtn_ = nullptr;
    wxButton* sortAlphaBtn_ = nullptr;
    wxButton* checkAllBtn_ = nullptr;
    wxButton* uncheckAllBtn_ = nullptr;

    // Log & action controls
    wxGauge* progressGauge_ = nullptr;
    wxStaticText* statsLabel_ = nullptr;
    wxTextCtrl* logTextCtrl_ = nullptr;
    wxButton* renderBtn_ = nullptr;
    wxButton* stopBtn_ = nullptr;
    wxButton* previewBtn_ = nullptr;
    wxButton* openFolderBtn_ = nullptr;
    wxButton* clearLogBtn_ = nullptr;
    wxButton* saveLogBtn_ = nullptr;

    wxSplitterWindow* splitter_ = nullptr;
    wxTimer pulseTimer_;

    std::vector<VideoFileItem> videoItems_;
    std::filesystem::path toolsDir_;
    AllToolsStatus toolsStatus_;
    RenderEngine engine_;
    ProcessRunner toolRunner_;
    std::thread workerThread_;
    std::atomic<bool> isRendering_{false};
    std::atomic<bool> cancelTools_{false};
};

} // namespace filmrender
