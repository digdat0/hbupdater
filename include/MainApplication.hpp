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
    pu::ui::elm::TextBlock::Ref title;
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
    void AddRow(const std::string &left, const std::string &right,
                pu::ui::Color lclr, pu::ui::Color rclr);
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

    void Toast(const std::string &msg);
    void ToastErr(const std::string &msg);
    bool Confirm(const std::string &title, const std::string &msg);

    void Refresh();              // rebuild the list from the loaded config
    void CheckOne(int idx);      // query GitHub for one app's latest release
    void CheckAll();
    bool UpdateOne(int idx);     // download + install if an update is available
    void AddApp();
    void HandleInput(u64 down, u64 held);
};
