#include <MainApplication.hpp>

int main(int argc, char **argv) {
    // hbloader passes the launched .nro path as argv[0]; used for self-update.
    MainApplication::SetLaunchPath((argc > 0 && argv[0]) ? argv[0] : "");

    // Apply a self-update staged on a previous run. If it applied + queued a
    // chainload, exit now so hbloader launches the freshly-updated nro.
    if (MainApplication::ApplyPendingUpdate()) {
        return 0;
    }

    auto opts = pu::ui::render::RendererInitOptions(
        SDL_INIT_EVERYTHING, pu::ui::render::RendererHardwareFlags);
    opts.UseImage(pu::ui::render::ImgAllFlags);
    opts.SetPlServiceType(PlServiceType_User);
    opts.AddDefaultAllSharedFonts();
    opts.SetInputPlayerCount(1);
    opts.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    opts.AddInputNpadIdType(HidNpadIdType_Handheld);
    opts.AddInputNpadIdType(HidNpadIdType_No1);

    auto renderer = pu::ui::render::Renderer::New(opts);
    auto main = MainApplication::New(renderer);

    const auto rc = main->Load();
    if (R_FAILED(rc)) {
        diagAbortWithResult(rc);
    }
    main->Show();
    return 0;
}
