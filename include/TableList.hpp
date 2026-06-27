#pragma once

// A lightweight scrolling list with real columns and an optional header row.
// Three logical cells per row: c0 (name, left-aligned, flex width, truncated
// with "..."), c1 (e.g. version) and c2 (e.g. status) as fixed right-aligned
// columns. Plutonium's built-in Menu packs a row into one label and can't form
// aligned columns, so this renders each cell itself. Navigation is driven by
// the app (MoveBy / SetSelected); OnInput is a no-op so selection has a single
// source of truth. Visible cells are cached as textures and only re-rendered
// when the window or content changes.

#include <pu/Plutonium>
#include <string>
#include <vector>

class TableList : public pu::ui::elm::Element {
  public:
    struct Row {
        std::string c0, c1, c2;
        pu::ui::Color clr0, clr1, clr2;
        float progress; // 0..1 draws a progress bar; <0 = none
    };

  private:
    // Default member initializers are load-bearing: RowCache rc; (trailing empty
    // rows) and the hdr_cache member are default-constructed, and freeCell()
    // would otherwise DeleteTexture an uninitialized garbage pointer -> crash.
    struct Cell {
        pu::sdl2::Texture tex = nullptr;
        s32 w = 0;
        s32 h = 0;
    };
    struct RowCache {
        Cell c0, c1, c2;
    };

    s32 x, y, w, row_h;
    s32 rows_visible;
    s32 sel;
    s32 scroll_top;
    pu::ui::Color row_bg, row_alt_bg, focus_bg, scroll_clr, hdr_bg, hdr_fg;
    s32 col1_w, col2_w;
    std::string font, hdr_font;
    std::vector<Row> rows;

    bool has_header;
    std::string h0, h1, h2;

    std::vector<RowCache> cache; // visible data window
    RowCache hdr_cache;
    bool hdr_dirty;
    s32 cache_top;
    bool dirty;

    static constexpr s32 PadX = 26;
    static constexpr s32 GAP = 18;
    static constexpr s32 HDR_H = 44;
    static constexpr s32 DEF_COL1_W = 300;
    static constexpr s32 DEF_COL2_W = 340;

    // Column geometry, relative to the element's x.
    s32 c2_right() const { return this->w - PadX; }
    s32 c2_left() const { return this->w - PadX - this->col2_w; }
    s32 c1_right() const { return this->c2_left() - GAP; }
    s32 c1_left() const { return this->c1_right() - this->col1_w; }
    s32 c0_max() const {
        s32 m = this->c1_left() - GAP - PadX;
        return m < 60 ? 60 : m;
    }
    s32 data_top() const { return this->has_header ? HDR_H : 0; }
    s32 data_rows() const {
        s32 h = this->row_h * this->rows_visible - this->data_top();
        s32 n = h / this->row_h;
        return n < 1 ? 1 : n;
    }

    void EnsureVisible() {
        const s32 vis = this->data_rows();
        if (this->sel < this->scroll_top) {
            this->scroll_top = this->sel;
        } else if (this->sel >= this->scroll_top + vis) {
            this->scroll_top = this->sel - vis + 1;
        }
        s32 maxtop = (s32)this->rows.size() - vis;
        if (maxtop < 0) {
            maxtop = 0;
        }
        if (this->scroll_top > maxtop) {
            this->scroll_top = maxtop;
        }
        if (this->scroll_top < 0) {
            this->scroll_top = 0;
        }
    }

    // RenderText returns a raw SDL_Texture* that must be freed explicitly, or
    // every cache rebuild leaks GPU memory.
    static void freeCell(Cell &c) {
        if (c.tex) {
            pu::ui::render::DeleteTexture(c.tex);
            c.tex = nullptr;
        }
    }
    void FreeCache() {
        for (auto &rc : this->cache) {
            freeCell(rc.c0);
            freeCell(rc.c1);
            freeCell(rc.c2);
        }
        this->cache.clear();
    }
    void FreeHeader() {
        freeCell(this->hdr_cache.c0);
        freeCell(this->hdr_cache.c1);
        freeCell(this->hdr_cache.c2);
    }

    Cell mk(const std::string &font_, const std::string &text,
            pu::ui::Color clr, s32 maxw) {
        Cell c{nullptr, 0, 0};
        if (!text.empty()) {
            c.tex = maxw > 0
                        ? pu::ui::render::RenderText(font_, text, clr, (u32)maxw)
                        : pu::ui::render::RenderText(font_, text, clr);
            c.w = pu::ui::render::GetTextureWidth(c.tex);
            c.h = pu::ui::render::GetTextureHeight(c.tex);
        }
        return c;
    }

    void RebuildHeader() {
        this->FreeHeader();
        if (this->has_header) {
            this->hdr_cache.c0 = mk(this->hdr_font, this->h0, this->hdr_fg, c0_max());
            this->hdr_cache.c1 = mk(this->hdr_font, this->h1, this->hdr_fg, this->col1_w);
            this->hdr_cache.c2 = mk(this->hdr_font, this->h2, this->hdr_fg, this->col2_w);
        }
        this->hdr_dirty = false;
    }

    void RebuildCache() {
        this->FreeCache();
        const s32 vis = this->data_rows();
        for (s32 i = 0; i < vis; i++) {
            s32 ridx = this->scroll_top + i;
            RowCache rc;
            if (ridx >= 0 && ridx < (s32)this->rows.size()) {
                Row &r = this->rows[ridx];
                rc.c1 = mk(this->font, r.c1, r.clr1, this->col1_w);
                rc.c2 = mk(this->font, r.c2, r.clr2, this->col2_w);
                // Single-column rows (log lines, messages) get the full width.
                bool single = r.c1.empty() && r.c2.empty();
                rc.c0 = mk(this->font, r.c0, r.clr0,
                           single ? (this->w - 2 * PadX) : this->c0_max());
            }
            this->cache.push_back(rc);
        }
        this->cache_top = this->scroll_top;
        this->dirty = false;
    }

  public:
    TableList(const s32 x, const s32 y, const s32 w, const s32 row_h,
              const s32 rows_visible)
        : x(x), y(y), w(w), row_h(row_h), rows_visible(rows_visible), sel(0),
          scroll_top(0), row_bg(22, 23, 27, 255), row_alt_bg(28, 30, 36, 255),
          focus_bg(28, 122, 116, 255), scroll_clr(80, 86, 100, 255),
          hdr_bg(16, 17, 21, 255), hdr_fg(150, 157, 172, 255),
          col1_w(DEF_COL1_W), col2_w(DEF_COL2_W), has_header(false),
          hdr_dirty(true), cache_top(-1), dirty(true) {
        this->font = pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::MediumLarge);
        this->hdr_font = pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium);
    }
    PU_SMART_CTOR(TableList)

    ~TableList() {
        this->FreeCache();
        this->FreeHeader();
    }

    void SetHeaders(const std::string &a, const std::string &b,
                    const std::string &c) {
        this->h0 = a;
        this->h1 = b;
        this->h2 = c;
        this->has_header = true;
        this->hdr_dirty = true;
        this->dirty = true;
    }
    void ClearHeaders() {
        this->has_header = false;
        this->hdr_dirty = true;
        this->dirty = true;
    }
    void SetColumnWidths(s32 c1, s32 c2) {
        this->col1_w = c1;
        this->col2_w = c2;
        this->hdr_dirty = true;
        this->dirty = true;
    }
    void ResetColumnWidths() {
        this->SetColumnWidths(DEF_COL1_W, DEF_COL2_W);
    }

    void Clear() {
        this->rows.clear();
        this->sel = 0;
        this->scroll_top = 0;
        this->dirty = true;
    }
    void AddRow(const std::string &left, const pu::ui::Color lclr) {
        this->rows.push_back(Row{left, "", "", lclr, lclr, lclr, -1.0f});
        this->dirty = true;
    }
    // Back-compat: a name + a single right-hand status column.
    void AddRow2(const std::string &left, const std::string &right,
                 const pu::ui::Color lclr, const pu::ui::Color rclr,
                 const float progress = -1.0f) {
        this->rows.push_back(Row{left, "", right, lclr, rclr, rclr, progress});
        this->dirty = true;
    }
    // Full three-column row: name, middle (version), right (status).
    void AddRow3(const std::string &name, const std::string &mid,
                 const std::string &right, const pu::ui::Color nclr,
                 const pu::ui::Color mclr, const pu::ui::Color rclr,
                 const float progress = -1.0f) {
        this->rows.push_back(
            Row{name, mid, right, nclr, mclr, rclr, progress});
        this->dirty = true;
    }

    s32 Count() { return (s32)this->rows.size(); }
    s32 GetSelected() { return this->sel; }
    s32 RowsVisible() { return this->data_rows(); }
    void SetSelected(const s32 i) {
        s32 n = (s32)this->rows.size();
        if (n <= 0) {
            this->sel = 0;
            this->scroll_top = 0;
            return;
        }
        this->sel = i < 0 ? 0 : (i >= n ? n - 1 : i);
        this->EnsureVisible();
    }
    void MoveBy(const s32 d) { this->SetSelected(this->sel + d); }
    void Step(const s32 d) {
        s32 n = (s32)this->rows.size();
        if (n <= 0) {
            return;
        }
        s32 i = this->sel + d;
        if (i < 0) {
            i = n - 1;
        } else if (i >= n) {
            i = 0;
        }
        this->SetSelected(i);
    }

    s32 GetX() override { return this->x; }
    s32 GetY() override { return this->y; }
    s32 GetWidth() override { return this->w; }
    s32 GetHeight() override { return this->row_h * this->rows_visible; }

    void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 rx,
                  const s32 ry) override {
        if (this->hdr_dirty) {
            this->RebuildHeader();
        }
        if (this->dirty || this->cache_top != this->scroll_top) {
            this->RebuildCache();
        }

        // Header strip.
        if (this->has_header) {
            drawer->RenderRectangleFill(this->hdr_bg, rx, ry, this->w, HDR_H);
            s32 cy = ry + (HDR_H - 28) / 2;
            if (this->hdr_cache.c0.tex) {
                drawer->RenderTexture(this->hdr_cache.c0.tex, rx + PadX,
                                      ry + (HDR_H - this->hdr_cache.c0.h) / 2);
            }
            if (this->hdr_cache.c1.tex) {
                drawer->RenderTexture(
                    this->hdr_cache.c1.tex,
                    rx + this->c1_right() - this->hdr_cache.c1.w,
                    ry + (HDR_H - this->hdr_cache.c1.h) / 2);
            }
            if (this->hdr_cache.c2.tex) {
                drawer->RenderTexture(
                    this->hdr_cache.c2.tex,
                    rx + this->c2_right() - this->hdr_cache.c2.w,
                    ry + (HDR_H - this->hdr_cache.c2.h) / 2);
            }
            (void)cy;
        }

        const s32 top = this->data_top();
        const s32 vis = this->data_rows();
        for (s32 i = 0; i < vis; i++) {
            s32 ridx = this->scroll_top + i;
            s32 rowy = ry + top + i * this->row_h;
            bool has = (ridx >= 0 && ridx < (s32)this->rows.size());
            pu::ui::Color bg =
                (has && ridx == this->sel)
                    ? this->focus_bg
                    : ((ridx % 2) ? this->row_alt_bg : this->row_bg);
            drawer->RenderRectangleFill(bg, rx, rowy, this->w, this->row_h);
            if (!has) {
                continue;
            }
            float prog = this->rows[ridx].progress;
            if (prog >= 0.0f) {
                if (prog > 1.0f) {
                    prog = 1.0f;
                }
                s32 bh = 6;
                s32 by = rowy + this->row_h - bh - 4;
                s32 bx = rx + PadX;
                s32 bw = this->w - 2 * PadX;
                drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 120), bx, by,
                                            bw, bh);
                drawer->RenderRectangleFill(pu::ui::Color(120, 225, 150, 255), bx,
                                            by, (s32)(bw * prog), bh);
            }
            RowCache &rc = this->cache[i];
            if (rc.c2.tex) {
                drawer->RenderTexture(rc.c2.tex, rx + this->c2_right() - rc.c2.w,
                                      rowy + (this->row_h - rc.c2.h) / 2);
            }
            if (rc.c1.tex) {
                drawer->RenderTexture(rc.c1.tex, rx + this->c1_right() - rc.c1.w,
                                      rowy + (this->row_h - rc.c1.h) / 2);
            }
            if (rc.c0.tex) {
                drawer->RenderTexture(rc.c0.tex, rx + PadX,
                                      rowy + (this->row_h - rc.c0.h) / 2);
            }
        }

        // Scrollbar thumb when the data overflows.
        s32 n = (s32)this->rows.size();
        if (n > vis) {
            s32 track_h = this->row_h * vis;
            s32 sb_w = 6;
            s32 sb_x = rx + this->w - sb_w;
            s32 thumb_h = (s32)((double)track_h * vis / n);
            if (thumb_h < 32) {
                thumb_h = 32;
            }
            s32 maxtop = n - vis;
            s32 thumb_y = ry + top +
                          (maxtop > 0 ? (s32)((double)(track_h - thumb_h) *
                                              this->scroll_top / maxtop)
                                      : 0);
            drawer->RenderRectangleFill(this->scroll_clr, sb_x, thumb_y, sb_w,
                                        thumb_h);
        }
    }

    void OnInput(const u64, const u64, const u64,
                 const pu::ui::TouchPoint) override {
        // Navigation is driven by the application (single source of truth).
    }
};
