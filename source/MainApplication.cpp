#include <MainApplication.hpp>
#include <pu/ui/extras/extras_Toast.hpp>
#include "version.h"

#include <string>
#include <vector>

extern "C" {
#include "config.h"
#include "net.h"
#include "update.h"
#include "fsutil.h"
#include <switch.h>
#include <string.h>
}

// ---- backend state --------------------------------------------------------
static AppsConfig g_cfg;
static std::vector<std::string> g_status; // per-app display status
static std::vector<std::string> g_latest; // per-app latest tag ("" = unknown)
static std::vector<std::string> g_url;     // per-app latest .nro asset url
static std::vector<int> g_state;           // 0 none,1 uptodate,2 update,3 fail
static std::string g_launch_path;

// ---- helpers --------------------------------------------------------------
static bool prompt(const char *guide, const char *initial, char *out,
                   size_t out_sz) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    if (guide) {
        swkbdConfigSetGuideText(&kbd, guide);
    }
    if (initial) {
        swkbdConfigSetInitialText(&kbd, initial);
    }
    Result rc = swkbdShow(&kbd, out, out_sz);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0] != '\0';
}

static pu::ui::Color state_color(int st) {
    switch (st) {
    case 1: return pu::ui::Color(130, 225, 150, 255); // up to date (green)
    case 2: return pu::ui::Color(240, 210, 120, 255); // update available (amber)
    case 3: return pu::ui::Color(240, 110, 110, 255); // failed (red)
    default: return pu::ui::Color(170, 175, 185, 255); // not checked (gray)
    }
}

// ---- MainLayout -----------------------------------------------------------
MainLayout::MainLayout() : Layout::Layout() {
    this->SetBackgroundColor(pu::ui::Color(12, 12, 14, 255));
    const s32 sw = (s32)pu::ui::render::ScreenWidth;
    const s32 sh = (s32)pu::ui::render::ScreenHeight;

    this->header = pu::ui::elm::Rectangle::New(0, 0, sw, 110,
                                               pu::ui::Color(33, 64, 124, 255));
    this->Add(this->header);
    this->title = pu::ui::elm::TextBlock::New(45, 28, "Homebrew Updater");
    this->title->SetColor(pu::ui::Color(255, 255, 255, 255));
    this->Add(this->title);
    this->status = pu::ui::elm::TextBlock::New(sw - 360, 36, "");
    this->status->SetColor(pu::ui::Color(210, 222, 245, 255));
    this->Add(this->status);

    const s32 footer_h = 64;
    const s32 list_y = 118;
    const s32 row_h = 84;
    const s32 rows = (sh - list_y - footer_h) / row_h;
    this->list = TableList::New(0, list_y, sw, row_h, rows);
    this->Add(this->list);

    this->footer = pu::ui::elm::Rectangle::New(0, sh - footer_h, sw, footer_h,
                                               pu::ui::Color(22, 42, 80, 255));
    this->Add(this->footer);
    for (int i = 0; i < 8; i++) {
        auto seg = pu::ui::elm::TextBlock::New(0, sh - footer_h + 14, "");
        seg->SetColor(pu::ui::Color(206, 216, 238, 255));
        this->Add(seg);
        this->footer_segs.push_back(seg);
    }
}

void MainLayout::SetTitle(const std::string &t) { this->title->SetText(t); }
void MainLayout::SetStatus(const std::string &t) { this->status->SetText(t); }
void MainLayout::SetFooter(const std::string &t) {
    std::vector<std::string> segs;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && t[i] == ' ') {
            i++;
        }
        if (i >= t.size()) {
            break;
        }
        size_t end = t.size();
        for (size_t j = i; j + 1 < t.size(); j++) {
            if (t[j] == ' ' && t[j + 1] == ' ') {
                end = j;
                break;
            }
        }
        segs.push_back(t.substr(i, end - i));
        i = end;
    }
    const s32 sw = (s32)pu::ui::render::ScreenWidth;
    const s32 margin = 30;
    int n = (int)segs.size();
    for (int k = 0; k < (int)this->footer_segs.size(); k++) {
        if (k < n) {
            this->footer_segs[k]->SetText(segs[k]);
            s32 cell = (sw - 2 * margin) / (n > 0 ? n : 1);
            s32 center = margin + cell * k + cell / 2;
            this->footer_segs[k]->SetX(center -
                                       this->footer_segs[k]->GetWidth() / 2);
        } else {
            this->footer_segs[k]->SetText("");
        }
    }
}
void MainLayout::ClearList() { this->list->Clear(); }
void MainLayout::AddRow(const std::string &left, const std::string &right,
                        pu::ui::Color lclr, pu::ui::Color rclr) {
    this->list->AddRow2(left, right, lclr, rclr);
}
s32 MainLayout::Sel() { return this->list->GetSelected(); }
void MainLayout::SetSel(s32 i) { this->list->SetSelected(i); }
s32 MainLayout::Count() { return this->list->Count(); }
void MainLayout::Step(s32 d) { this->list->Step(d); }
void MainLayout::PageUp() { this->list->MoveBy(-this->list->RowsVisible()); }
void MainLayout::PageDown() { this->list->MoveBy(this->list->RowsVisible()); }

// ---- MainApplication ------------------------------------------------------
void MainApplication::SetLaunchPath(const std::string &p) { g_launch_path = p; }

static MainLayout::Ref g_layout;

void MainApplication::Toast(const std::string &msg) {
    auto tb = pu::ui::elm::TextBlock::New(0, 0, msg);
    tb->SetColor(pu::ui::Color(255, 255, 255, 255));
    auto t = pu::ui::extras::Toast::New(tb, pu::ui::Color(46, 120, 78, 240));
    this->StartOverlayWithTimeout(t, 1500);
}
void MainApplication::ToastErr(const std::string &msg) {
    auto tb = pu::ui::elm::TextBlock::New(0, 0, msg);
    tb->SetColor(pu::ui::Color(255, 255, 255, 255));
    auto t = pu::ui::extras::Toast::New(tb, pu::ui::Color(160, 52, 52, 240));
    this->StartOverlayWithTimeout(t, 1800);
}
bool MainApplication::Confirm(const std::string &title, const std::string &msg) {
    int r = this->CreateShowDialog(title, msg, {"Cancel", "Yes"}, false);
    return r == 1;
}

void MainApplication::Refresh() {
    g_status.resize(g_cfg.count);
    g_latest.resize(g_cfg.count);
    g_url.resize(g_cfg.count);
    g_state.resize(g_cfg.count);

    g_layout->SetTitle(std::string("Homebrew Updater   (v") + APP_VERSION_STR +
                       ")");
    char st[64];
    snprintf(st, sizeof(st), "%d app%s", g_cfg.count, g_cfg.count == 1 ? "" : "s");
    g_layout->SetStatus(st);
    g_layout->SetFooter(
        "A update  X check all  Y add  - remove  ZL/ZR page  + exit");

    s32 keep = g_layout->Sel();
    g_layout->ClearList();
    for (int i = 0; i < g_cfg.count; i++) {
        std::string right = g_status[i];
        if (right.empty()) {
            right = g_cfg.apps[i].version[0]
                        ? std::string("installed ") + g_cfg.apps[i].version
                        : std::string("not checked");
        }
        g_layout->AddRow(g_cfg.apps[i].name, right,
                         pu::ui::Color(232, 234, 240, 255),
                         state_color(g_state[i]));
    }
    if (g_cfg.count == 0) {
        g_layout->AddRow("(no apps tracked - press Y to add)", "",
                         pu::ui::Color(170, 175, 185, 255),
                         pu::ui::Color(170, 175, 185, 255));
    }
    g_layout->SetSel(keep);
}

void MainApplication::CheckOne(int idx) {
    if (idx < 0 || idx >= g_cfg.count) {
        return;
    }
    char tag[64] = {0}, url[1024] = {0};
    if (!update_fetch_latest(g_cfg.apps[idx].repo, tag, sizeof(tag), url,
                             sizeof(url))) {
        g_state[idx] = 3;
        g_status[idx] = "check failed";
        g_latest[idx].clear();
        g_url[idx].clear();
        return;
    }
    g_latest[idx] = tag;
    g_url[idx] = url;
    const char *inst = g_cfg.apps[idx].version;
    if (inst[0] && version_cmp(inst, tag) >= 0) {
        g_state[idx] = 1;
        g_status[idx] = std::string("up to date ") + tag;
    } else {
        g_state[idx] = 2;
        g_status[idx] = (inst[0] ? std::string(inst) + " -> " : std::string()) +
                        tag;
    }
}

void MainApplication::CheckAll() {
    for (int i = 0; i < g_cfg.count; i++) {
        this->CheckOne(i);
    }
    this->Refresh();
    this->Toast("Checked all");
}

bool MainApplication::UpdateOne(int idx) {
    if (idx < 0 || idx >= g_cfg.count) {
        return false;
    }
    if (g_latest[idx].empty()) {
        this->CheckOne(idx); // not checked yet
    }
    if (g_state[idx] != 2) {
        return false; // nothing to do / failed
    }
    fs_mkdir_p(DL_TMP_DIR);
    std::string tmp = std::string(DL_TMP_DIR) + "/update.nro";
    long code = 0;
    if (!http_download(g_url[idx].c_str(), tmp.c_str(), NULL, NULL, NULL, 0,
                       &code)) {
        remove(tmp.c_str());
        g_state[idx] = 3;
        g_status[idx] = "download failed";
        return false;
    }
    if (!fs_move(tmp.c_str(), g_cfg.apps[idx].path)) {
        remove(tmp.c_str());
        g_state[idx] = 3;
        g_status[idx] = "install failed";
        return false;
    }
    snprintf(g_cfg.apps[idx].version, sizeof(g_cfg.apps[idx].version), "%s",
             g_latest[idx].c_str());
    apps_save(&g_cfg);
    g_state[idx] = 1;
    g_status[idx] = std::string("updated to ") + g_latest[idx];
    return true;
}

void MainApplication::AddApp() {
    char repo[128] = {0}, path[512] = {0}, name[64] = {0};
    if (!prompt("GitHub repo (owner/name)", nullptr, repo, sizeof(repo))) {
        return;
    }
    if (!prompt("SD path to the .nro (sdmc:/switch/.../App.nro)", nullptr, path,
                sizeof(path))) {
        return;
    }
    prompt("Display name (optional)", repo, name, sizeof(name));
    if (apps_add(&g_cfg, name, repo, path)) {
        apps_save(&g_cfg);
        this->Refresh();
        this->Toast("Added");
    } else {
        this->ToastErr("Could not add");
    }
}

void MainApplication::HandleInput(u64 down, u64 held) {
    const u64 NAV_UP = HidNpadButton_Up | HidNpadButton_StickLUp;
    const u64 NAV_DOWN = HidNpadButton_Down | HidNpadButton_StickLDown;
    if (down & NAV_DOWN) {
        g_layout->Step(1);
    }
    if (down & NAV_UP) {
        g_layout->Step(-1);
    }
    {
        static int hold = 0;
        int dir = (held & NAV_DOWN) ? 1 : (held & NAV_UP) ? -1 : 0;
        if (dir == 0) {
            hold = 0;
        } else if (++hold > 22 && ((hold - 22) % 3) == 0) {
            g_layout->Step(dir);
        }
    }
    if (down & HidNpadButton_ZL) {
        g_layout->PageUp();
    }
    if (down & HidNpadButton_ZR) {
        g_layout->PageDown();
    }
    if (down & HidNpadButton_Plus) {
        this->Close();
        return;
    }
    if (down & HidNpadButton_X) {
        if (g_cfg.count > 0) {
            g_layout->SetStatus("checking...");
            this->CheckAll();
        }
        return;
    }
    if (down & HidNpadButton_Y) {
        this->AddApp();
        return;
    }
    s32 i = g_layout->Sel();
    if (down & HidNpadButton_A) {
        if (i >= 0 && i < g_cfg.count) {
            this->CheckOne(i);
            this->Refresh();
            if (g_state[i] == 2) {
                if (this->Confirm("Update",
                                  std::string("Update ") + g_cfg.apps[i].name +
                                      " to " + g_latest[i] + "?")) {
                    if (this->UpdateOne(i)) {
                        this->Toast("Updated");
                    } else {
                        this->ToastErr("Update failed");
                    }
                    this->Refresh();
                }
            } else if (g_state[i] == 1) {
                this->Toast("Already up to date");
            } else {
                this->ToastErr("Check failed");
            }
        }
    } else if (down & HidNpadButton_Minus) {
        if (i >= 0 && i < g_cfg.count) {
            if (this->Confirm("Remove", std::string("Stop tracking ") +
                                            g_cfg.apps[i].name + "?")) {
                apps_remove(&g_cfg, i);
                apps_save(&g_cfg);
                this->Refresh();
                this->Toast("Removed");
            }
        }
    }
}

void MainApplication::OnLoad() {
    romfsInit();
    net_init();
    apps_load(&g_cfg);

    g_layout = MainLayout::New();
    this->LoadLayout(g_layout);
    this->Refresh();

    this->SetOnInput([&](const u64 down, const u64 up, const u64 held,
                         const pu::ui::TouchPoint touch) {
        (void)up;
        (void)touch;
        this->HandleInput(down, held);
    });
}
