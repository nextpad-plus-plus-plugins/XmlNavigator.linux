// XmlNavigator — Linux (GTK4) port. Mirrors XmlNavigator.mm.
//
// Shows the structure of an XML file in a tree view; clicking a node jumps
// the caret to its opening tag. Right-click menu: Go To Node Start/End,
// Go To Content Start/End, Select Node, Select Content. The hand-rolled
// permissive tokenizer (byte offsets, tolerant of ill-formed XML) is ported
// verbatim from the macOS build, with XNNode as a plain C++ struct.
//
// UI: GtkTreeView + GtkTreeStore (the host's own side panels use the same
// widget family) with native expanders; filter row on top (all tokens must
// match, case-insensitive, subtree-aware); Ctrl +/-/0 font zoom.

#include "plugin.h"
#include "Scintilla.h"

#include <string>
#include <vector>
#include <memory>
#include <cstring>

#define NPP_EXPORT __attribute__((visibility("default")))

// ---------------------------------------------------------------------------
// Plugin identification
// ---------------------------------------------------------------------------
static const char *PLUGIN_NAME = "XML Navigator";
static const int NB_FUNC = 2;
static FuncItem funcItem[NB_FUNC];
static NppData nppData;

enum FuncIdx { IdxShowNavigator = 0, IdxAbout };

// ---------------------------------------------------------------------------
// Scintilla / NPP helpers
// ---------------------------------------------------------------------------
extern "C" intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                                uintptr_t wParam, intptr_t lParam);

static long npp(unsigned int msg, unsigned long w = 0, long l = 0) {
    return nppData.hostMsg(msg, w, l);
}
static void *currentScintilla() {
    int which = 0;
    return (void *)(intptr_t)npp(NPPM_GETCURRENTSCINTILLA, 0, (long)(intptr_t)&which);
}
static intptr_t sci(unsigned int msg, uintptr_t w = 0, intptr_t l = 0) {
    void *h = currentScintilla();
    return h ? scintilla_view_send_message(h, msg, w, l) : 0;
}

static std::string currentDocumentText() {
    void *h = currentScintilla();
    if (!h) return std::string();
    intptr_t length = scintilla_view_send_message(h, SCI_GETLENGTH, 0, 0);
    if (length <= 0) return std::string();
    std::string buf((size_t)length + 1, '\0');
    scintilla_view_send_message(h, SCI_GETTEXT, (uintptr_t)(length + 1),
                                (intptr_t)buf.data());
    buf.resize((size_t)length);
    return buf;
}

// ---------------------------------------------------------------------------
// XML tree model (plain C++ — was the ObjC XNNode)
// ---------------------------------------------------------------------------
struct XmlNode {
    std::string name, localName, displayName, comment;
    std::vector<std::pair<std::string, std::string>> attributes;
    intptr_t nodeStart = -1;     // position of '<' of opening tag
    intptr_t nodeEnd = -1;       // position AFTER '>' of closing tag (or '/>')
    intptr_t contentStart = -1;  // position after '>' of opening tag
    intptr_t contentEnd = -1;    // position of '<' of closing tag
    bool isEmpty = false;
    int depth = 0;
    XmlNode *parent = nullptr;
    std::vector<std::unique_ptr<XmlNode>> children;
};

// All filter tokens must appear in displayName, case-insensitive (Windows
// semantics: order-independent AND).
static bool nameMatchesFilter(const XmlNode *n, const std::vector<std::string> &tokens) {
    if (tokens.empty()) return true;
    gchar *hay = g_utf8_casefold(n->displayName.c_str(), -1);
    bool all = true;
    for (const auto &t : tokens) {
        if (t.empty()) continue;
        gchar *needle = g_utf8_casefold(t.c_str(), -1);
        if (!strstr(hay, needle)) { g_free(needle); all = false; break; }
        g_free(needle);
    }
    g_free(hay);
    return all;
}
static bool subtreeMatchesFilter(const XmlNode *n, const std::vector<std::string> &tokens) {
    if (tokens.empty()) return true;
    if (nameMatchesFilter(n, tokens)) return true;
    for (const auto &c : n->children)
        if (subtreeMatchesFilter(c.get(), tokens)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// XML tokenizer — ported verbatim (position semantics match XmlParser.cs)
// ---------------------------------------------------------------------------
namespace xmlscan {

static inline bool isNameStart(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           ch == '_' || ch == ':' || ch >= 0x80;
}
static inline bool isNameChar(unsigned char ch) {
    return isNameStart(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
}

static size_t skipWS(const std::string &src, size_t pos) {
    while (pos < src.size()) {
        unsigned char c = (unsigned char)src[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos;
        else break;
    }
    return pos;
}

static std::string readName(const std::string &src, size_t &pos) {
    size_t start = pos;
    if (pos >= src.size() || !isNameStart((unsigned char)src[pos])) return std::string();
    ++pos;
    while (pos < src.size() && isNameChar((unsigned char)src[pos])) ++pos;
    return src.substr(start, pos - start);
}

static std::string readAttributeValue(const std::string &src, size_t &pos) {
    if (pos >= src.size()) return std::string();
    char quote = src[pos];
    if (quote != '"' && quote != '\'') return std::string();
    ++pos;
    std::string out;
    while (pos < src.size() && src[pos] != quote) {
        if (src[pos] == '&') {
            size_t semi = src.find(';', pos);
            if (semi != std::string::npos && semi - pos <= 8) {
                std::string ent = src.substr(pos + 1, semi - pos - 1);
                if      (ent == "amp")  { out += '&';  pos = semi + 1; continue; }
                else if (ent == "lt")   { out += '<';  pos = semi + 1; continue; }
                else if (ent == "gt")   { out += '>';  pos = semi + 1; continue; }
                else if (ent == "quot") { out += '"';  pos = semi + 1; continue; }
                else if (ent == "apos") { out += '\''; pos = semi + 1; continue; }
            }
        }
        out += src[pos++];
    }
    if (pos < src.size() && src[pos] == quote) ++pos;
    return out;
}

static std::string readComment(const std::string &src, size_t &pos) {
    pos += 4;
    size_t start = pos;
    size_t end = src.find("-->", pos);
    std::string body;
    if (end == std::string::npos) {
        pos = src.size();
    } else {
        body = src.substr(start, end - start);
        pos = end + 3;
    }
    return body;
}

static void skipCDATA(const std::string &src, size_t &pos) {
    pos += 9;
    size_t end = src.find("]]>", pos);
    pos = (end == std::string::npos) ? src.size() : end + 3;
}
static void skipPI(const std::string &src, size_t &pos) {
    pos += 2;
    size_t end = src.find("?>", pos);
    pos = (end == std::string::npos) ? src.size() : end + 2;
}
static void skipDoctype(const std::string &src, size_t &pos) {
    pos += 2;
    int bracketDepth = 0;
    while (pos < src.size()) {
        char c = src[pos];
        if (c == '[') ++bracketDepth;
        else if (c == ']') --bracketDepth;
        else if (c == '>' && bracketDepth <= 0) { ++pos; return; }
        ++pos;
    }
}

static std::unique_ptr<XmlNode> parse(const std::string &src) {
    std::unique_ptr<XmlNode> ownedRoot;      // final single root
    XmlNode *root = nullptr;
    XmlNode *synthesizedRoot = nullptr;
    XmlNode *current = nullptr;
    std::string pendingComment;
    bool havePendingComment = false;

    size_t pos = 0;
    const size_t N = src.size();

    while (pos < N) {
        if (src[pos] != '<') { ++pos; continue; }

        if (pos + 3 < N && src[pos + 1] == '!' && src[pos + 2] == '-' && src[pos + 3] == '-') {
            pendingComment = readComment(src, pos);
            havePendingComment = true;
            continue;
        }
        if (pos + 8 < N && src.compare(pos, 9, "<![CDATA[") == 0) { skipCDATA(src, pos); continue; }
        if (pos + 1 < N && src[pos + 1] == '?') { skipPI(src, pos); continue; }
        if (pos + 1 < N && src[pos + 1] == '!') { skipDoctype(src, pos); continue; }

        // End tag?
        if (pos + 1 < N && src[pos + 1] == '/') {
            intptr_t tagStart = (intptr_t)pos;
            pos += 2;
            readName(src, pos);
            pos = skipWS(src, pos);
            if (pos < N && src[pos] == '>') ++pos;
            else {
                size_t gt = src.find('>', pos);
                pos = (gt == std::string::npos) ? N : gt + 1;
            }
            intptr_t tagEnd = (intptr_t)pos;

            if (current) {
                current->contentEnd = tagStart;
                current->nodeEnd = tagEnd;
                current = current->parent;   // pop even on mismatch (malformed)
            }
            continue;
        }

        // Start tag
        intptr_t openStart = (intptr_t)pos;
        ++pos;
        std::string name = readName(src, pos);
        if (name.empty()) continue;   // loose '<' in content

        auto node = std::make_unique<XmlNode>();
        XmlNode *np = node.get();
        np->name = name;
        // Split on the FIRST colon (namespace prefix), matching the macOS build.
        size_t colon = name.find(':');
        np->localName = (colon == std::string::npos) ? name : name.substr(colon + 1);
        np->nodeStart = openStart;
        if (havePendingComment) {
            np->comment = pendingComment;
            havePendingComment = false;
        }

        // Attributes
        while (pos < N) {
            pos = skipWS(src, pos);
            if (pos >= N) break;
            char c = src[pos];
            if (c == '/' || c == '>') break;
            std::string attrName = readName(src, pos);
            if (attrName.empty()) { ++pos; continue; }
            pos = skipWS(src, pos);
            std::string attrValue;
            if (pos < N && src[pos] == '=') {
                ++pos;
                pos = skipWS(src, pos);
                attrValue = readAttributeValue(src, pos);
            }
            np->attributes.emplace_back(attrName, attrValue);
        }

        // Self-closing?
        bool selfClosing = false;
        if (pos < N && src[pos] == '/') { selfClosing = true; ++pos; }
        if (pos < N && src[pos] == '>') ++pos;
        intptr_t openEnd = (intptr_t)pos;

        np->contentStart = openEnd;
        np->isEmpty = selfClosing;
        if (selfClosing) {
            np->contentEnd = openEnd;
            np->nodeEnd = openEnd;
        }

        // Display name: "localName [attrValue1] [attrValue2]…"
        np->displayName = np->localName;
        if (!np->attributes.empty()) {
            np->displayName += " ";
            bool first = true;
            for (const auto &a : np->attributes) {
                if (!first) np->displayName += " ";
                np->displayName += "[" + a.second + "]";
                first = false;
            }
        }

        // Parent / depth wiring
        if (current) {
            np->parent = current;
            np->depth = current->depth + 1;
            current->children.push_back(std::move(node));
        } else {
            np->depth = 0;
            if (!root) {
                root = np;
                ownedRoot = std::move(node);
            } else {
                if (!synthesizedRoot) {
                    auto synth = std::make_unique<XmlNode>();
                    synth->name = synth->localName = synth->displayName = "(fragments)";
                    synth->nodeStart = root->nodeStart;
                    synth->contentStart = root->nodeStart;
                    root->parent = synth.get();
                    root->depth = 1;
                    synth->children.push_back(std::move(ownedRoot));
                    synthesizedRoot = synth.get();
                    ownedRoot = std::move(synth);
                    root = synthesizedRoot;
                }
                np->parent = synthesizedRoot;
                np->depth = 1;
                synthesizedRoot->children.push_back(std::move(node));
            }
        }

        if (!selfClosing) current = np;
    }

    return ownedRoot;
}

} // namespace xmlscan

// ---------------------------------------------------------------------------
// Scintilla navigation — SCI_GRABFOCUS so the caret is visible after a
// panel-originated jump (same reasoning as the macOS build).
// ---------------------------------------------------------------------------
static void gotoPosition(intptr_t position) {
    if (position < 0) return;
    sci(SCI_GOTOPOS, (uintptr_t)position);
    sci(SCI_SCROLLCARET);
    sci(SCI_GRABFOCUS);
}
static void setSelection(intptr_t startPos, intptr_t endPos) {
    if (startPos < 0 || endPos < 0) return;
    sci(SCI_SETSELECTIONSTART, (uintptr_t)startPos);
    sci(SCI_SETSELECTIONEND,   (uintptr_t)endPos);
    sci(SCI_SCROLLCARET);
    sci(SCI_GRABFOCUS);
}

// ---------------------------------------------------------------------------
// Navigator panel — GtkBox: filter row + GtkTreeView in a scrolled window.
// ---------------------------------------------------------------------------
enum { COL_TEXT = 0, COL_NODE, N_COLS };

static GtkWidget     *sPanelBox   = NULL;
static GtkWidget     *sFilter     = NULL;
static GtkWidget     *sTree       = NULL;
static GtkTreeStore  *sStore      = NULL;
static GtkWidget     *sCtxMenu    = NULL;   // GtkPopoverMenu
static GSimpleActionGroup *sActions = NULL;
static GtkCssProvider *sFontProv  = NULL;

static std::unique_ptr<XmlNode> sRoot;
static std::vector<std::string> sFilterTokens;
static XmlNode *sCtxNode = NULL;            // node under the context menu
static double sFontSize = 11.0;             // Ctrl +/-/0 zoom
static long g_panelHandle = 0;

static bool panelIsShown() {
    return sPanelBox && gtk_widget_get_mapped(sPanelBox);
}

static std::string rowText(const XmlNode *n) {
    if (!n->comment.empty()) return n->displayName + "  // " + n->comment;
    return n->displayName;
}

static void appendChildren(XmlNode *n, GtkTreeIter *iterParent) {
    for (const auto &cptr : n->children) {
        XmlNode *c = cptr.get();
        if (!sFilterTokens.empty() && !subtreeMatchesFilter(c, sFilterTokens))
            continue;
        GtkTreeIter it;
        gtk_tree_store_append(sStore, &it, iterParent);
        gtk_tree_store_set(sStore, &it,
                           COL_TEXT, rowText(c).c_str(),
                           COL_NODE, (gpointer)c, -1);
        appendChildren(c, &it);
    }
}

static void rebuildStore() {
    // Clear the store FIRST, then the tree may be replaced by the caller —
    // no stale node pointers can remain in view callbacks.
    gtk_tree_store_clear(sStore);
    if (!sRoot) return;
    XmlNode *root = sRoot.get();
    if (!sFilterTokens.empty() && !subtreeMatchesFilter(root, sFilterTokens))
        return;
    GtkTreeIter it;
    gtk_tree_store_append(sStore, &it, NULL);
    gtk_tree_store_set(sStore, &it,
                       COL_TEXT, rowText(root).c_str(),
                       COL_NODE, (gpointer)root, -1);
    appendChildren(root, &it);

    if (!sFilterTokens.empty()) {
        gtk_tree_view_expand_all(GTK_TREE_VIEW(sTree));
    } else {
        // Expand just the root row (macOS expands the single top-level item).
        GtkTreePath *p = gtk_tree_path_new_first();
        gtk_tree_view_expand_row(GTK_TREE_VIEW(sTree), p, FALSE);
        gtk_tree_path_free(p);
    }
}

static void reloadPanel() {
    if (!panelIsShown()) return;
    std::string text = currentDocumentText();
    auto newRoot = xmlscan::parse(text);
    // The context-menu target points into the OLD tree — clear it before that
    // tree is freed, or a still-open popover's action would dereference freed
    // memory (a reload can fire from a notification while the menu is up).
    sCtxNode = NULL;
    gtk_tree_store_clear(sStore);       // drop refs to the OLD tree first
    sRoot = std::move(newRoot);
    rebuildStore();
}

static XmlNode *nodeAtIter(GtkTreeIter *it) {
    gpointer p = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(sStore), it, COL_NODE, &p, -1);
    return (XmlNode *)p;
}
static XmlNode *selectedNode() {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sTree));
    GtkTreeIter it;
    GtkTreeModel *model = NULL;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return NULL;
    return nodeAtIter(&it);
}

// Single click on a row → jump to node start (macOS AfterSelect semantics).
static void on_tree_click(GtkGestureClick *g, int, double x, double y, gpointer) {
    GtkTreeView *tv = GTK_TREE_VIEW(sTree);
    GtkTreePath *path = NULL;
    // x/y are widget coords; convert to bin-window coords for the hit test.
    int bx = 0, by = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, (int)x, (int)y, &bx, &by);
    if (!gtk_tree_view_get_path_at_pos(tv, bx, by, &path, NULL, NULL, NULL)) return;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(sStore), &it, path)) {
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(tv), &it);
        XmlNode *n = nodeAtIter(&it);
        if (n) gotoPosition(n->nodeStart);
    }
    gtk_tree_path_free(path);
    (void)g;
}

// Keyboard Enter → same jump.
static void on_row_activated(GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *, gpointer) {
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(sStore), &it, path)) {
        XmlNode *n = nodeAtIter(&it);
        if (n) gotoPosition(n->nodeStart);
    }
}

// ── Context menu actions ──────────────────────────────────────────────
static void ctx_do(const char *what) {
    XmlNode *n = sCtxNode ? sCtxNode : selectedNode();
    if (!n) return;
    if      (!strcmp(what, "node-start"))    gotoPosition(n->nodeStart);
    else if (!strcmp(what, "node-end"))      gotoPosition(n->nodeEnd);
    else if (!strcmp(what, "content-start")) { if (!n->isEmpty) gotoPosition(n->contentStart); }
    else if (!strcmp(what, "content-end"))   { if (!n->isEmpty) gotoPosition(n->contentEnd); }
    else if (!strcmp(what, "select-node"))   setSelection(n->nodeStart, n->nodeEnd);
    else if (!strcmp(what, "select-content")){ if (!n->isEmpty) setSelection(n->contentStart, n->contentEnd); }
}
static void on_ctx_action(GSimpleAction *a, GVariant *, gpointer) {
    ctx_do(g_action_get_name(G_ACTION(a)));
}

static void ctxSetEnabled(const char *name, bool enabled) {
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(sActions), name);
    if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), enabled);
}

// Right-click: select the row under the pointer, gate content actions,
// pop the GtkPopoverMenu at the pointer.
static void on_tree_right_click(GtkGestureClick *, int, double x, double y, gpointer) {
    GtkTreeView *tv = GTK_TREE_VIEW(sTree);
    GtkTreePath *path = NULL;
    int bx = 0, by = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, (int)x, (int)y, &bx, &by);
    sCtxNode = NULL;
    if (gtk_tree_view_get_path_at_pos(tv, bx, by, &path, NULL, NULL, NULL)) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(sStore), &it, path)) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(tv), &it);
            sCtxNode = nodeAtIter(&it);
        }
        gtk_tree_path_free(path);
    }
    if (!sCtxNode) return;
    bool hasContent = !sCtxNode->isEmpty && sCtxNode->contentEnd > sCtxNode->contentStart;
    ctxSetEnabled("content-start",  hasContent);
    ctxSetEnabled("content-end",    hasContent);
    ctxSetEnabled("select-content", hasContent);

    // The popover is parented to the panel BOX (parenting it to the
    // GtkTreeView trips gtk_css_node_insert_after assertions — the legacy
    // treeview manages its own css children); translate the click point.
    graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y), out = in;
    if (!gtk_widget_compute_point(sTree, sPanelBox, &in, &out)) out = in;
    GdkRectangle r = { (int)out.x, (int)out.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(sCtxMenu), &r);
    gtk_popover_popup(GTK_POPOVER(sCtxMenu));
}

// ── Filter ────────────────────────────────────────────────────────────
static void on_filter_changed(GtkSearchEntry *entry, gpointer) {
    const char *raw = gtk_editable_get_text(GTK_EDITABLE(entry));
    bool hadFilter = !sFilterTokens.empty();
    sFilterTokens.clear();
    if (raw && *raw) {
        gchar **parts = g_strsplit_set(raw, " \t", -1);
        for (int i = 0; parts[i]; i++)
            if (*parts[i]) sFilterTokens.push_back(parts[i]);
        g_strfreev(parts);
    }
    rebuildStore();
    // Clearing an active filter keeps the tree fully expanded (macOS parity —
    // its outline view preserves expansion by item identity across the clear).
    if (hadFilter && sFilterTokens.empty())
        gtk_tree_view_expand_all(GTK_TREE_VIEW(sTree));
}

// ── Font-size zoom (Ctrl +/-/0 while focus is inside the panel) ───────
static void applyFontSize() {
    if (!sFontProv) {
        sFontProv = gtk_css_provider_new();
        gtk_style_context_add_provider(gtk_widget_get_style_context(sTree),
            GTK_STYLE_PROVIDER(sFontProv), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gchar *css = g_strdup_printf("treeview { font-size: %dpx; }", (int)sFontSize);
    gtk_css_provider_load_from_string(sFontProv, css);
    g_free(css);
}
static gboolean on_panel_key(GtkEventControllerKey *, guint keyval, guint,
                             GdkModifierType state, gpointer) {
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    switch (keyval) {
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
            sFontSize = MIN(28.0, sFontSize + 1); applyFontSize(); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
            sFontSize = MAX(8.0, sFontSize - 1); applyFontSize(); return TRUE;
        case GDK_KEY_0: case GDK_KEY_KP_0:
            sFontSize = 11.0; applyFontSize(); return TRUE;
    }
    return FALSE;
}

static void ensurePanel() {
    if (sPanelBox) return;

    sPanelBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    sFilter = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(sFilter), "Filter nodes…");
    gtk_widget_set_margin_start(sFilter, 6);
    gtk_widget_set_margin_end(sFilter, 6);
    gtk_widget_set_margin_top(sFilter, 4);
    gtk_widget_set_margin_bottom(sFilter, 4);
    g_signal_connect(sFilter, "search-changed", G_CALLBACK(on_filter_changed), NULL);
    gtk_box_append(GTK_BOX(sPanelBox), sFilter);

    sStore = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_POINTER);
    sTree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sStore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(sTree), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(sTree), FALSE);
    gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(sTree), 4);

    GtkCellRenderer *rend = gtk_cell_renderer_text_new();
    g_object_set(rend, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "", rend, "text", COL_TEXT, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(sTree), col);

    // Single-click jump (button 1, bubble phase so expander clicks still work)
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "released", G_CALLBACK(on_tree_click), NULL);
    gtk_widget_add_controller(sTree, GTK_EVENT_CONTROLLER(click));
    g_signal_connect(sTree, "row-activated", G_CALLBACK(on_row_activated), NULL);

    // Right-click context menu
    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_tree_right_click), NULL);
    gtk_widget_add_controller(sTree, GTK_EVENT_CONTROLLER(rclick));

    static const char *actionNames[] = {
        "node-start", "node-end", "content-start",
        "content-end", "select-node", "select-content" };
    sActions = g_simple_action_group_new();
    for (const char *nm : actionNames) {
        GSimpleAction *a = g_simple_action_new(nm, NULL);
        g_signal_connect(a, "activate", G_CALLBACK(on_ctx_action), NULL);
        g_action_map_add_action(G_ACTION_MAP(sActions), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(sTree, "xn", G_ACTION_GROUP(sActions));

    GMenu *menu = g_menu_new();
    GMenu *sec1 = g_menu_new();
    g_menu_append(sec1, "Go to Node Start",    "xn.node-start");
    g_menu_append(sec1, "Go to Node End",      "xn.node-end");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec1));
    GMenu *sec2 = g_menu_new();
    g_menu_append(sec2, "Go to Content Start", "xn.content-start");
    g_menu_append(sec2, "Go to Content End",   "xn.content-end");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec2));
    GMenu *sec3 = g_menu_new();
    g_menu_append(sec3, "Select Node",         "xn.select-node");
    g_menu_append(sec3, "Select Content",      "xn.select-content");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec3));
    sCtxMenu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(sec1); g_object_unref(sec2); g_object_unref(sec3);
    g_object_unref(menu);
    gtk_popover_set_has_arrow(GTK_POPOVER(sCtxMenu), FALSE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), sTree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(sPanelBox), scroll);

    // Parent the popover to the panel box (NOT the treeview — see the
    // css-node note in on_tree_right_click). The action group lives on the
    // treeview; popovers resolve actions through their parent chain, and
    // the box is an ancestor of the treeview, so move the group here.
    gtk_widget_insert_action_group(sPanelBox, "xn", G_ACTION_GROUP(sActions));
    gtk_widget_set_parent(sCtxMenu, sPanelBox);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_panel_key), NULL);
    gtk_widget_add_controller(sPanelBox, keys);
}

// ---------------------------------------------------------------------------
// Toggle / show / hide via the host docking API (Linux ABI: lParam=widget,
// wParam=title). No floating fallback — the host has docking since GAP-49.
// ---------------------------------------------------------------------------
static gboolean postShowReloadCb(gpointer) {
    reloadPanel();
    return G_SOURCE_REMOVE;
}

static void cmdShowNavigator() {
    ensurePanel();
    if (g_panelHandle == 0) {
        g_panelHandle = npp(NPPM_DMM_REGISTERPANEL,
                            (unsigned long)(uintptr_t)"XML Navigator",
                            (long)(intptr_t)sPanelBox);
        if (g_panelHandle == 0) {
            g_warning("[XmlNavigator] panel registration failed");
            return;
        }
    }
    if (panelIsShown()) {
        npp(NPPM_DMM_HIDEPANEL, (unsigned long)g_panelHandle, 0);
    } else {
        npp(NPPM_DMM_SHOWPANEL, (unsigned long)g_panelHandle, 0);
        // Widget maps on the next frame — reload once it can answer
        // panelIsShown() truthfully.
        g_timeout_add(100, postShowReloadCb, NULL);
    }
}

static void cmdAbout() {
    GtkAlertDialog *a = gtk_alert_dialog_new("XML Navigator");
    gtk_alert_dialog_set_detail(a,
        "Version 1.0 (Linux port)\n\n"
        "Tree-view navigator for XML documents. Click any node to jump\n"
        "to its position in the editor; right-click for more options.\n\n"
        "Original Windows plugin by Christian Grasser (archived).");
    gtk_alert_dialog_show(a, GTK_WINDOW(nppData.nppHandle));
    g_object_unref(a);
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
static void registerToolbarIcon() {
    static bool done = false;
    if (done) return;
    done = true;
    static char iconPath[2304];
    char buf[2048] = {0};
    npp(NPPM_GETPLUGINHOMEPATH, 0, (long)(intptr_t)buf);
    if (!buf[0]) return;
    g_snprintf(iconPath, sizeof iconPath,
               "%s/XmlNavigator/resources/toolbar.png", buf);
    npp(NPPM_ADDTOOLBARICON_FORDARKMODE,
        (unsigned long)funcItem[IdxShowNavigator].cmdID, (long)(intptr_t)iconPath);
}

extern "C" NPP_EXPORT void setInfo(NppData data) {
    nppData = data;
    memset(funcItem, 0, sizeof(funcItem));
    g_strlcpy(funcItem[IdxShowNavigator].itemName, "Show XML Navigator",
              sizeof funcItem[IdxShowNavigator].itemName);
    funcItem[IdxShowNavigator].pFunc = cmdShowNavigator;
    g_strlcpy(funcItem[IdxAbout].itemName, "About",
              sizeof funcItem[IdxAbout].itemName);
    funcItem[IdxAbout].pFunc = cmdAbout;
}

extern "C" NPP_EXPORT const char *getName(void) { return PLUGIN_NAME; }
extern "C" NPP_EXPORT FuncItem *getFuncsArray(int *nbF) { *nbF = NB_FUNC; return funcItem; }

extern "C" NPP_EXPORT void beNotified(SCNotification *n) {
    if (!n) return;
    switch (n->nmhdr.code) {
        case NPPN_TBMODIFICATION:
        case NPPN_READY:
            registerToolbarIcon();
            break;
        case NPPN_BUFFERACTIVATED:
        case NPPN_FILESAVED:
            if (panelIsShown()) reloadPanel();
            break;
        case SCN_MODIFIED:
            // Self-filter: this host forwards every SCN_MODIFIED flavour
            // (see PORTING_NOTES.md) — react to text changes only.
            if (panelIsShown() &&
                (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
                reloadPanel();
            }
            break;
        case NPPN_SHUTDOWN:
            if (g_panelHandle > 0) {
                npp(NPPM_DMM_UNREGISTERPANEL, (unsigned long)g_panelHandle, 0);
                g_panelHandle = 0;
            }
            break;
        default: break;
    }
}

extern "C" NPP_EXPORT long messageProc(unsigned int, unsigned long, long) { return 1; }
extern "C" NPP_EXPORT int isUnicode(void) { return 1; }

// ── self-test hook (headless verification of the tokenizer) ────────────────
extern "C" NPP_EXPORT int xmlnavigator_selftest(void) {
    auto root = xmlscan::parse(
        "<?xml version=\"1.0\"?>\n"
        "<!-- top comment -->\n"
        "<root a=\"1\">\n"
        "  <child id=\"first\">text</child>\n"
        "  <empty/>\n"
        "  <ns:elem>x</ns:elem>\n"
        "</root>\n");
    if (!root) return 1;
    if (root->localName != "root") return 2;
    if (root->comment != " top comment ") return 3;
    if (root->children.size() != 3) return 4;
    const XmlNode *child = root->children[0].get();
    if (child->displayName != "child [first]") return 5;
    if (child->isEmpty) return 6;
    // content of <child id="first"> is exactly "text"
    if (child->contentEnd - child->contentStart != 4) return 7;
    if (!root->children[1]->isEmpty) return 8;
    if (root->children[2]->localName != "elem") return 9;
    // fragment handling
    auto frag = xmlscan::parse("<a/><b/>");
    if (!frag || frag->displayName != "(fragments)" || frag->children.size() != 2) return 10;
    return 0;
}
