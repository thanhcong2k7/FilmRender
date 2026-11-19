#pragma once

#include "ProcessRunner.h"
#include "ToolManager.h"
#include "YtDlpUtils.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace filmrender {

class Mp3DownloaderDialog final : public wxDialog {
public:
    Mp3DownloaderDialog(wxWindow* parent,
                        const std::filesystem::path& toolsDir,
                        const std::filesystem::path& initialAudioDir,
                        const std::string& customFfmpegPath);
    ~Mp3DownloaderDialog() override;

    std::filesystem::path GetAudioDir() const;
    bool DidDownloadMp3s() const { return downloadedMp3s_; }

private:
    enum {
        ID_BrowseAudioDir = wxID_HIGHEST + 200,
        ID_RecheckTools,
        ID_DownloadMissingTools,
        ID_UpdateAllTools,
        ID_AddUrlToQueue,
        ID_ClearCompleted,
        ID_ClearQueue,
        ID_StartMp3Download,
        ID_StopTask,
        ID_PulseTimer
    };

    enum WorkerEventKind {
        WorkerLog = 1,
        WorkerProgress = 2,
        WorkerToolsChanged = 3,
        WorkerFinished = 4,
        WorkerItemFetched = 5,
        WorkerItemState = 6,
        WorkerItemProgress = 7,
        WorkerThumbReady = 8
    };

    enum class ItemState {
        Queued,
        Downloading,
        Done,
        Skipped,
        Failed
    };

    struct QueueRowWidgets {
        wxPanel* cardPanel = nullptr;
        wxStaticBitmap* thumbBitmap = nullptr;
        wxStaticText* titleLabel = nullptr;
        wxStaticText* metaLabel = nullptr;
        wxTextCtrl* filenameCtrl = nullptr;
        wxGauge* progressGauge = nullptr;
        wxStaticText* statusLabel = nullptr;
        wxButton* removeBtn = nullptr;
    };

    struct QueueItem {
        std::uint64_t uid = 0;
        YtVideoEntry entry;
        std::string currentFilename;
        ItemState state = ItemState::Queued;
        double progress = 0.0;
        std::string statusText = "Queued";
        QueueRowWidgets widgets;
    };

    struct ItemStatePayload {
        std::uint64_t uid = 0;
        ItemState state = ItemState::Queued;
        double progress = 0.0;
        std::string statusText;
    };

    struct ThumbReadyPayload {
        std::uint64_t uid = 0;
        std::string tempImagePath;
    };

    void CreateControls();
    void RefreshToolStatuses();
    void UpdateControlStates();
    void UpdateQueueSummary();
    void AppendLog(const wxString& line);

    bool HasDuplicateEntry(const YtVideoEntry& entry) const;
    void AddQueueItemRow(const YtVideoEntry& entry);
    void RemoveQueueItem(std::uint64_t uid);
    QueueItem* FindItemByUid(std::uint64_t uid);

    void EnqueueThumbnailFetch(std::uint64_t uid, const std::string& thumbUrl);
    void StartThumbnailWorker();
    void StopThumbnailWorker();

    void StartToolDownload(const std::vector<ToolId>& toolsToDownload);

    void OnBrowseAudioDir(wxCommandEvent& evt);
    void OnRecheckTools(wxCommandEvent& evt);
    void OnDownloadMissingTools(wxCommandEvent& evt);
    void OnUpdateAllTools(wxCommandEvent& evt);
    void OnAddUrlToQueue(wxCommandEvent& evt);
    void OnClearCompleted(wxCommandEvent& evt);
    void OnClearQueue(wxCommandEvent& evt);
    void OnStartMp3Download(wxCommandEvent& evt);
    void OnStopTask(wxCommandEvent& evt);
    void OnPulseTimer(wxTimerEvent& evt);
    void OnWorkerEvent(wxThreadEvent& evt);
    void OnClose(wxCloseEvent& evt);

    std::filesystem::path toolsDir_;
    std::string customFfmpegPath_;
    AllToolsStatus toolsStatus_;

    // Tools status widgets
    wxStaticText* ytDlpStatusLabel_ = nullptr;
    wxStaticText* bunStatusLabel_ = nullptr;
    wxStaticText* ffmpegStatusLabel_ = nullptr;
    wxStaticText* toolsPathLabel_ = nullptr;
    wxButton* recheckBtn_ = nullptr;
    wxButton* downloadMissingBtn_ = nullptr;
    wxButton* updateAllBtn_ = nullptr;

    // Input widgets
    wxTextCtrl* audioDirCtrl_ = nullptr;
    wxButton* browseAudioBtn_ = nullptr;
    wxTextCtrl* urlCtrl_ = nullptr;
    wxButton* addUrlBtn_ = nullptr;
    wxCheckBox* playlistCheck_ = nullptr;
    wxCheckBox* noOverwritesCheck_ = nullptr;

    // Queue widgets
    wxStaticText* queueSummaryLabel_ = nullptr;
    wxButton* clearCompletedBtn_ = nullptr;
    wxButton* clearQueueBtn_ = nullptr;
    wxScrolledWindow* queueScroll_ = nullptr;
    wxBoxSizer* queueItemsSizer_ = nullptr;
    wxBitmap placeholderThumb_;

    // Log & action widgets
    wxSplitterWindow* splitter_ = nullptr;
    wxGauge* progressGauge_ = nullptr;
    wxStaticText* statusLabel_ = nullptr;
    wxTextCtrl* logTextCtrl_ = nullptr;
    wxButton* downloadMp3Btn_ = nullptr;
    wxButton* stopBtn_ = nullptr;
    wxButton* closeBtn_ = nullptr;

    wxTimer pulseTimer_;
    ProcessRunner runner_;
    std::thread workerThread_;
    std::atomic<bool> isBusy_{false};
    std::atomic<bool> cancelRequested_{false};
    bool downloadedMp3s_ = false;

    std::vector<QueueItem> items_;
    std::mutex filenamesMutex_;
    std::map<std::uint64_t, std::string> liveFilenames_;
    std::uint64_t nextUid_ = 1;
    std::size_t fetchAddedCount_ = 0;
    std::size_t fetchDupCount_ = 0;
    bool fetchingUrl_ = false;

    // Background thumbnail fetcher
    std::thread thumbThread_;
    ProcessRunner thumbRunner_;
    std::mutex thumbMutex_;
    std::condition_variable thumbCv_;
    std::deque<std::pair<std::uint64_t, std::string>> thumbQueue_;
    bool thumbStop_ = false;
};

} // namespace filmrender
