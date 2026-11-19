// desktop-app/Source/ui/MixMasterEngine/RenderTreePanel.cpp
#include "RenderTreePanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RevisionReportView.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu {

namespace {
constexpr int kInfoLineH = 16;
} // namespace

RenderTreePanel::RenderTreePanel(IAppController& c) : PanelCard(c, "Render Tree")
{
    setExplainer(TRANS("Render Tree"),
                 TRANS("Every render you apply becomes a node here, hung off the render it was built on. Undo a "
                       "step and apply a different move and the old path isn't thrown away - it forks into a "
                       "branch you can still return to. Click any node to jump playback (and the A/B \"B\" side) "
                       "back to it.\n\nRight-click a node to name or tag it (a restore point), branch from it "
                       "(new renders are then recorded on that branch), compare it with the current version, or "
                       "export its branch as a .khensu bundle. Versions saved in an earlier run are listed under "
                       "Saved versions; click one to make it the current audition version again."));
    setTitle(TRANS("Render tree"));
    setDescription(TRANS("Render history with names, tags and branches. Right-click a node for actions."));

    renderTreeListener_ = listen(controller_.renders(), this, &IRenders::addRenderTreeListener,
                                 &IRenders::removeRenderTreeListener, [this]() {
                                     syncVersions();
                                     repaint();
                                 });
}

RenderTreePanel::~RenderTreePanel()
{
    if (dialog_ != nullptr) dialog_->exitModalState(0);
}

juce::String RenderTreePanel::sessionId() const
{
    const auto* s = controller_.getActiveSession();
    return s != nullptr ? juce::String(s->id) : juce::String();
}

void RenderTreePanel::resetForSession()
{
    versionsByPath_.clear();
    branches_.clear();
    activeBranch_.clear();
    syncedSessionId_.clear();
    status_.clear();
    repaint();
}

void RenderTreePanel::setStatus(const juce::String& s)
{
    status_ = s;
    repaint();
}

juce::String RenderTreePanel::headerText() const
{
    juce::String t = TRANS("Branch: ") + (activeBranch_.isEmpty() ? TRANS("main") : activeBranch_);
    if (status_.isNotEmpty()) t << "   " << status_;
    return t;
}

// ---------------------------------------------------------------------------
// sync with the service
// ---------------------------------------------------------------------------

void RenderTreePanel::syncVersions()
{
    const auto sid = sessionId();
    if (sid.isEmpty()) return;
    if (syncInFlight_) {
        syncAgain_ = true; // the tree changed again while a sync was running
        return;
    }
    syncInFlight_ = true;
    syncAgain_ = false;
    syncedSessionId_ = sid;

    juce::Array<juce::var> nodes;
    const auto& tree = controller_.renders().renderTree();
    for (const auto& n : tree) {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty(apif::RenderNodeIn::path, n.path);
        o->setProperty(apif::RenderNodeIn::parent_path,
                       n.parent >= 0 && n.parent < (int)tree.size() ? tree[(size_t)n.parent].path : juce::String());
        nodes.add(juce::var(o.get()));
    }
    const auto branch = activeBranch_;
    juce::Component::SafePointer<RenderTreePanel> self(this);
    controller_.runInBackground([self, &work = controller_, sid, nodes, branch] {
        auto result = ipc::HttpServiceBridge{}.syncRenderVersions(sid, juce::var(nodes), branch);
        postIfCurrent(self, work, [sid, result](RenderTreePanel& panel) {
            if (sid != panel.sessionId()) return;
            if (result.is_err())
                panel.setStatus(TRANS("Versions not saved: ") + serviceErrorLine(result.error()));
            else
                panel.applyVersions(result.value());
            if (panel.syncAgain_) panel.syncVersions();
        }, [](RenderTreePanel& panel) { panel.syncInFlight_ = false; }); // reset busy state first
    });
}

void RenderTreePanel::applyVersions(const juce::var& response)
{
    versionsByPath_.clear();
    if (auto* arr = response[apif::RenderVersionList::versions].getArray()) {
        for (const auto& v : *arr) {
            VersionInfo info;
            info.id = v[apif::RenderVersion::id].toString();
            info.label = v[apif::RenderVersion::label].toString();
            info.branch = v[apif::RenderVersion::branch].toString();
            info.parentId = v[apif::RenderVersion::parent_id].toString();
            if (auto* tags = v[apif::RenderVersion::tags].getArray())
                for (const auto& t : *tags)
                    info.tags.add(t.toString());
            versionsByPath_[v[apif::RenderVersion::path].toString()] = info;
        }
    }
    branches_.clear();
    if (auto* arr = response[apif::RenderVersionList::branches].getArray())
        for (const auto& b : *arr)
            branches_.add(b[apif::RenderBranch::name].toString());
    repaint();
}

// ---------------------------------------------------------------------------
// layout + paint
// ---------------------------------------------------------------------------

void RenderTreePanel::rebuildLayout()
{
    layout_.clear();
    savedHeaderY_ = -1;
    const auto& tree = controller_.renders().renderTree();

    auto area = bodyArea();
    area.removeFromTop(28); // header/explainer strip drawn by PanelCard
    area.removeFromTop(kInfoLineH);
    const int indent = 16;
    const float dotR = 4.0f;
    int y = area.getY() + 4;

    for (int i = 0; i < (int)tree.size(); ++i) {
        int depth = 0;
        for (int p = tree[(size_t)i].parent; p >= 0; p = tree[(size_t)p].parent)
            if (++depth > 64) break; // guard against a malformed cycle

        const float cx = (float)(area.getX() + 8 + depth * indent);
        const float cy = (float)(y + rowH_ / 2);
        layout_.push_back({i, depth, {cx - dotR, cy - dotR, dotR * 2, dotR * 2}, {}});
        y += rowH_;
    }

    // Versions saved in an earlier run (not in the in-memory tree) whose file
    // still exists - one click restores them.
    bool header = false;
    for (const auto& [path, info] : versionsByPath_) {
        bool live = false;
        for (const auto& n : tree)
            live = live || n.path == path;
        if (live || !juce::File(path).existsAsFile()) continue;
        if (!header) {
            header = true;
            y += 4;
            savedHeaderY_ = y;
            y += 18;
        }
        const float cx = (float)(area.getX() + 8);
        const float cy = (float)(y + rowH_ / 2);
        layout_.push_back({-1, 0, {cx - dotR, cy - dotR, dotR * 2, dotR * 2}, path});
        y += rowH_;
    }
}

void RenderTreePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);

    const auto sid = sessionId();
    if (sid.isNotEmpty() && sid != syncedSessionId_ && !syncInFlight_) {
        // First paint for this session: record the tree and load names, tags,
        // branches and earlier-run versions. Deferred so paint() stays pure.
        if (syncedSessionId_.isNotEmpty()) {
            versionsByPath_.clear();
            branches_.clear();
            activeBranch_.clear();
        }
        syncedSessionId_ = sid;
        juce::Component::SafePointer<RenderTreePanel> self(this);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) self->syncVersions();
        });
    }

    rebuildLayout();

    auto info = bodyArea();
    info.removeFromTop(28);
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.setColour(activeBranch_.isEmpty() ? KhensuTheme::textTertiary : KhensuTheme::accentTeal);
    g.drawText(headerText(), info.removeFromTop(kInfoLineH).reduced(8, 0), juce::Justification::centredLeft, true);

    const auto& tree = controller_.renders().renderTree();
    if (layout_.empty()) {
        drawEmptyState(g, TRANS("No renders yet"), TRANS("Apply a chain to start the render tree."),
                       30 + kInfoLineH);
        return;
    }

    const int current = controller_.renders().currentRenderTreeNode();

    // Connectors first (parent dot -> child dot), so dots draw on top.
    g.setColour(KhensuTheme::borderActive);
    for (const auto& nl : layout_) {
        if (nl.index < 0) continue;
        const int parent = tree[(size_t)nl.index].parent;
        if (parent < 0) continue;
        const auto& pl = layout_[(size_t)parent]; // parent laid out earlier (lower index)
        const auto pc = pl.dot.getCentre();
        const auto cc = nl.dot.getCentre();
        juce::Path path;
        path.startNewSubPath(pc.x, pc.y);
        path.lineTo(pc.x, cc.y);
        path.lineTo(cc.x, cc.y);
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }

    if (savedHeaderY_ >= 0) {
        g.setFont(KhensuTheme::labelFont(10.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("Saved versions (earlier runs) - click to restore"), bodyArea().getX() + 8, savedHeaderY_,
                   getWidth() - 16, 16, juce::Justification::centredLeft, true);
    }

    for (const auto& nl : layout_) {
        const bool isCurrent = nl.index >= 0 && nl.index == current;
        const juce::String path = nl.index >= 0 ? tree[(size_t)nl.index].path : nl.savedPath;
        g.setColour(isCurrent ? KhensuTheme::accentTeal : KhensuTheme::textSecondary);
        if (isCurrent)
            g.fillEllipse(nl.dot.expanded(1.5f));
        else if (nl.index < 0)
            g.drawEllipse(nl.dot, 1.0f);
        else
            g.fillEllipse(nl.dot);

        juce::String name = nl.index >= 0 ? "R" + juce::String(nl.index + 1) + "  " : juce::String();
        name << juce::File(path).getFileNameWithoutExtension();
        juce::String extra;
        if (const auto it = versionsByPath_.find(path); it != versionsByPath_.end()) {
            const auto& v = it->second;
            if (v.label.isNotEmpty()) name = (nl.index >= 0 ? "R" + juce::String(nl.index + 1) + "  " : juce::String()) +
                                             v.label;
            if (v.branch.isNotEmpty()) extra << "  [" << v.branch << "]";
            if (!v.tags.isEmpty()) extra << "  #" << v.tags.joinIntoString(" #");
        }
        const int textX = (int)nl.dot.getRight() + 8;
        const int textY = (int)nl.dot.getCentreY() - 8;
        g.setFont(KhensuTheme::labelFont(10.0f));
        g.setColour(isCurrent ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
        const int nameW = juce::jmin(getWidth() - textX - 8,
                                     juce::GlyphArrangement::getStringWidthInt(KhensuTheme::labelFont(10.0f), name) + 4);
        g.drawText(name, textX, textY, nameW, 16, juce::Justification::centredLeft, true);
        if (extra.isNotEmpty()) {
            g.setColour(KhensuTheme::accentBlue);
            g.drawText(extra, textX + nameW, textY, getWidth() - textX - nameW - 8, 16,
                       juce::Justification::centredLeft, true);
        }
    }
}

// ---------------------------------------------------------------------------
// interaction
// ---------------------------------------------------------------------------

void RenderTreePanel::mouseDown(const juce::MouseEvent& e)
{
    for (const auto& nl : layout_) {
        juce::Rectangle<float> row(0.0f, nl.dot.getY() - 3.0f, (float)getWidth(), (float)rowH_);
        if (!row.contains(e.position)) continue;
        if (nl.index < 0) {
            restoreSaved(nl.savedPath);
            return;
        }
        if (e.mods.isPopupMenu()) {
            showNodeMenu(nl.index, controller_.renders().renderTree()[(size_t)nl.index].path);
            return;
        }
        controller_.renders().restoreRenderTreeNode(nl.index);
        return;
    }
    PanelCard::mouseDown(e);
}

void RenderTreePanel::restoreSaved(const juce::String& path)
{
    const auto sid = sessionId();
    if (sid.isEmpty() || !juce::File(path).existsAsFile()) {
        setStatus(TRANS("That version's file is missing."));
        return;
    }
    controller_.registerAppliedRender(path, sid);
    setStatus(TRANS("Restored ") + juce::File(path).getFileName());
}

void RenderTreePanel::showNodeMenu(int treeIndex, const juce::String& path)
{
    VersionInfo v;
    if (const auto it = versionsByPath_.find(path); it != versionsByPath_.end()) v = it->second;
    const bool known = v.id.isNotEmpty();

    juce::PopupMenu m;
    m.addItem(1, TRANS("Restore (make current)"));
    m.addItem(2, TRANS("Name / tag..."), known);
    m.addItem(3, TRANS("Branch from here..."), known);
    if (activeBranch_.isNotEmpty()) m.addItem(4, TRANS("Switch to main line"));
    m.addSeparator();
    m.addItem(5, TRANS("Compare with current..."));
    m.addItem(6, TRANS("Export branch as bundle..."), known);
    if (!known) m.addSectionHeader(TRANS("Saving this version... try again in a moment"));

    juce::Component::SafePointer<RenderTreePanel> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                    [self, treeIndex, path, v](int r) {
                        if (self == nullptr || r == 0) return;
                        switch (r) {
                            case 1: self->controller_.renders().restoreRenderTreeNode(treeIndex); break;
                            case 2: self->promptLabel(v); break;
                            case 3: self->promptBranch(treeIndex, v); break;
                            case 4:
                                self->activeBranch_.clear();
                                self->setStatus(TRANS("New renders go to the main line."));
                                break;
                            case 5: self->compareWithCurrent(path); break;
                            case 6: self->exportBranch(v.branch); break;
                            default: break;
                        }
                    });
}

void RenderTreePanel::promptLabel(const VersionInfo& v)
{
    dialog_ = std::make_unique<juce::AlertWindow>(TRANS("Name / tag version"),
                                                  TRANS("A name makes this version a restore point. Tags are "
                                                        "comma-separated (for example: client, radio edit)."),
                                                  juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor("label", v.label, TRANS("Name"));
    dialog_->addTextEditor("tags", v.tags.joinIntoString(", "), TRANS("Tags"));
    dialog_->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<RenderTreePanel> self(this);
    const auto id = v.id;
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self, id](int r) {
        if (self == nullptr || self->dialog_ == nullptr) return;
        const auto label = self->dialog_->getTextEditorContents("label").trim();
        juce::StringArray tags;
        tags.addTokens(self->dialog_->getTextEditorContents("tags"), ",", "");
        tags.trim();
        tags.removeEmptyStrings();
        self->dialog_.reset();
        if (r != 1) return;
        juce::Array<juce::var> tagVars;
        for (const auto& t : tags)
            tagVars.add(t);
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty(apif::VersionUpdateRequest::label, label);
        body->setProperty(apif::VersionUpdateRequest::tags, juce::var(tagVars));
        const juce::var bodyVar(body.get());
        self->controller_.runInBackground([self, &work = self->controller_, id, bodyVar] {
            auto result = ipc::HttpServiceBridge{}.updateRenderVersion(id, bodyVar);
            postIfCurrent(self, work, [result](RenderTreePanel& panel) {
                if (result.is_err()) {
                    panel.setStatus(TRANS("Not saved: ") + serviceErrorLine(result.error()));
                    return;
                }
                panel.setStatus(TRANS("Version saved."));
                panel.syncVersions();
            });
        });
    }));
}

void RenderTreePanel::promptBranch(int treeIndex, const VersionInfo& v)
{
    dialog_ = std::make_unique<juce::AlertWindow>(
        TRANS("Branch from here"),
        TRANS("Name the new branch. This version becomes current and new renders are recorded on the branch."),
        juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor("name", {}, TRANS("Branch name"));
    dialog_->addButton(TRANS("Create"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<RenderTreePanel> self(this);
    const auto baseId = v.id;
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self, treeIndex, baseId](int r) {
        if (self == nullptr || self->dialog_ == nullptr) return;
        const auto name = self->dialog_->getTextEditorContents("name").trim();
        self->dialog_.reset();
        if (r != 1 || name.isEmpty()) return;
        const auto sid = self->sessionId();
        self->controller_.runInBackground([self, &work = self->controller_, sid, name, baseId, treeIndex] {
            auto result = ipc::HttpServiceBridge{}.createRenderBranch(sid, name, baseId);
            postIfCurrent(self, work, [name, treeIndex, result](RenderTreePanel& panel) {
                if (result.is_err()) {
                    panel.setStatus(TRANS("Branch not created: ") + serviceErrorLine(result.error()));
                    return;
                }
                panel.activeBranch_ = name;
                panel.setStatus(TRANS("New renders go to branch ") + name + ".");
                panel.controller_.renders().restoreRenderTreeNode(treeIndex);
                panel.syncVersions();
            });
        });
    }));
}

void RenderTreePanel::compareWithCurrent(const juce::String& path)
{
    juce::String a = path, b = controller_.currentAuditionPath();
    juce::String aLabel = juce::File(path).getFileName(), bLabel;
    if (b.isEmpty() || b == path) {
        // This node IS the current version: compare it with the original.
        a = controller_.activeSourcePath();
        aLabel = TRANS("Original");
        b = path;
        bLabel = juce::File(path).getFileName();
    } else {
        bLabel = TRANS("Current: ") + juce::File(b).getFileName();
    }
    if (a.isEmpty() || !juce::File(a).existsAsFile() || !juce::File(b).existsAsFile()) {
        setStatus(TRANS("Nothing to compare: a version's file is missing."));
        return;
    }
    RevisionReportView::show(controller_, a, b, aLabel, bLabel, this);
}

void RenderTreePanel::exportBranch(const juce::String& branch)
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr) return;
    const auto base = juce::File::createLegalFileName(juce::String(s->name).isNotEmpty() ? juce::String(s->name)
                                                                                         : juce::String("session")) +
                      (branch.isEmpty() ? juce::String("_main") : "_" + juce::File::createLegalFileName(branch));
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Export branch as bundle"),
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(base + ".khensu"),
        "*.khensu");
    juce::Component::SafePointer<RenderTreePanel> self(this);
    const auto sid = juce::String(s->id), sname = juce::String(s->name), source = controller_.activeSourcePath();
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [self, sid, sname, source, branch](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              auto file = fc.getResult();
                              if (file == juce::File()) return;
                              if (!file.hasFileExtension(".khensu")) file = file.withFileExtension(".khensu");
                              juce::DynamicObject::Ptr body = new juce::DynamicObject();
                              body->setProperty(apif::BundleExportRequest::session_id, sid);
                              body->setProperty(apif::BundleExportRequest::output_path, file.getFullPathName());
                              body->setProperty(apif::BundleExportRequest::session_name, sname);
                              body->setProperty(apif::BundleExportRequest::branch, branch);
                              body->setProperty(apif::BundleExportRequest::include_audio, true);
                              juce::Array<juce::var> sources;
                              if (source.isNotEmpty()) sources.add(source);
                              body->setProperty(apif::BundleExportRequest::source_paths, juce::var(sources));
                              const juce::var bodyVar(body.get());
                              self->setStatus(TRANS("Exporting bundle..."));
                              self->controller_.runLongInBackground([self, &work = self->controller_, bodyVar] {
                                  auto result = ipc::HttpServiceBridge{}.postCollab("/bundle/export", bodyVar);
                                  postIfCurrent(self, work, [result](RenderTreePanel& panel) {
                                      if (result.is_err()) {
                                          panel.setStatus(TRANS("Export failed: ") +
                                                          serviceErrorLine(result.error()));
                                          return;
                                      }
                                      panel.setStatus(
                                          TRANS("Exported ") +
                                          juce::File(result.value()[apif::BundleExportResponse::output_path]
                                                         .toString())
                                              .getFileName());
                                  }, [](RenderTreePanel& panel) { panel.setStatus({}); }); // reset busy state first
                              });
                          });
}

} // namespace khensu
