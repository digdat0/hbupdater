#pragma once

#include <pu/Plutonium>
#include <string>
#include <vector>
#include <switch.h>
#include "TableList.hpp"

class MainLayout : public pu::ui::Layout {
  private:
    pu::ui::elm::Rectangle::Ref header;
    pu::ui::elm::Rectangle::Ref footer;
    pu::ui::elm::TextBlock::Ref product; // fixed brand, far left
    pu::ui::elm::TextBlock::Ref title;   // page name, right of brand
    pu::ui::elm::TextBlock::Ref status;
    std::vector<pu::ui::elm::TextBlock::Ref> footer_segs;
    TableList::Ref list;

  public:
    MainLayout();
    PU_SMART_CTOR(MainLayout)

    void SetTitle(const std::string &t);
    void SetStatus(const std::string &t);
    void SetFooter(const std::string &t);
    void ClearList();
    void SetColumns(const std::string &a, const std::string &b,
                    const std::string &c);
    void ClearColumns();
    void AddRow(const std::string &left, const std::string &right,
                pu::ui::Color lclr, pu::ui::Color rclr);
    void AddRow3(const std::string &name, const std::string &ver,
                 const std::string &status, pu::ui::Color nclr,
                 pu::ui::Color vclr, pu::ui::Color sclr);
    s32 Sel();
    void SetSel(s32 i);
    s32 Count();
    void Step(s32 d);
    void PageUp();
    void PageDown();
};

class MainApplication : public pu::ui::Application {
  public:
    using Application::Application;
    PU_SMART_CTOR(MainApplication)

    void OnLoad() override;
    static void SetLaunchPath(const std::string &p);
    // Apply a staged self-update before the UI; true means it chainloaded the new
    // nro and main() should return.
    static bool ApplyPendingUpdate();

    void Toast(const std::string &msg);
    void ToastErr(const std::string &msg);
    bool Confirm(const std::string &title, const std::string &msg);

    void Refresh();              // rebuild the tracked-app list from the config
    void CheckAll();             // start a background check of every app
    void StartCheck(int idx, bool offer_update); // background check of one app
    void Tick();                 // per-frame: poll the worker, drive completion

    void OpenCatalog();          // Settings -> Supported apps (read-only browse)
    void CloseCatalog();         // back to Settings
    void RefreshCatalog();       // rebuild the supported-apps list

    void ReconcileInstalled();   // opt-out: rebuild home from installed apps

    void OpenExcluded();         // settings -> excluded-apps manager
    void RefreshExcluded();      // rebuild the excluded list
    void Unexclude();            // restore the selected excluded app

    void OpenSettings();         // enter the settings/toggles screen
    void RefreshSettings();      // rebuild the settings list view
    void ToggleSetting();        // flip the selected setting + save
    void UpdateCatalog();        // OTA: fetch the latest catalog from the repo
    void UpdateSelf();           // self-update HBUpdater from its own releases

    void OpenLogs();             // settings -> log picker
    void RefreshLogMenu();       // rebuild the log-picker list
    void OpenLog(int idx);       // display one log file's contents

    void OpenBackups(int appidx); // manage an app's backup snapshots
    void RefreshBackups();        // rebuild the backup list
    void RevertBackup();          // restore the selected snapshot
    void DeleteBackup();          // delete the selected snapshot
    void ClearBackups();          // delete all snapshots for the app

    void HandleInput(u64 down, u64 held);
};
