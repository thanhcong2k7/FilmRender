#include "MainFrame.h"

#include <wx/app.h>
#include <wx/image.h>

class FilmRenderApp final : public wxApp {
public:
    bool OnInit() override {
        wxInitAllImageHandlers();
        SetAppName("FilmRender");
        SetVendorName("FilmRender");

        auto* frame = new filmrender::MainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(FilmRenderApp);
