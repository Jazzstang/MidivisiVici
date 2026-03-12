/*
==============================================================================
FlatComboBox.cpp
------------------------------------------------------------------------------
Role du fichier:
- Implementation du composant combo "flat" utilise dans plusieurs modules UI.

Place dans l architecture:
- Composant UI pur (message thread), sans dependance directe au moteur audio.
- Peut etre relie a APVTS via FlatComboBoxParameterAttachment.

Flux principal:
1) La vue maintient une liste locale d items (texte + id).
2) Le clic gauche ouvre un PopupMenu JUCE.
3) La selection met a jour selectedId/selectedText puis notifie onChange.
4) Le clic droit peut etre reroute vers un menu contextuel custom.

Points de vigilance:
- Les IDs d item servent de contrat avec les attachements de parametres.
- Ne pas appeler onChange pendant les synchros silencieuses.
- Ce composant ne fait aucun traitement RT et ne doit jamais etre utilise
  depuis le thread audio.
==============================================================================
*/

#include "FlatComboBox.h"
#include "../PluginLookAndFeel.h"
#include "../PluginColours.h"

namespace
{
    // Reserved negative IDs used by FlatComboBox internal multi-select actions.
    constexpr int kActionSelectAll = -10001;
    constexpr int kActionInvert = -10002;
    constexpr int kActionClear = -10003;
}

FlatComboBox::FlatComboBox()
{
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);
}

// Helper de geometrie: dessine un rectangle a coins arrondis selectifs.
// Utilise pour la variante "corps + capsule fleche" du combo.
static void addSelectiveRoundedRect(juce::Path& path,
                                    juce::Rectangle<float> area,
                                    float radius,
                                    bool topLeft, bool topRight,
                                    bool bottomLeft, bool bottomRight)
{
    const float x = area.getX();
    const float y = area.getY();
    const float w = area.getWidth();
    const float h = area.getHeight();
    const float r = juce::jlimit(0.0f, juce::jmin(w * 0.5f, h * 0.5f), radius);

    path.startNewSubPath(x + (topLeft ? r : 0), y);

    if (topRight)
    {
        path.lineTo(x + w - r, y);
        path.quadraticTo(x + w, y, x + w, y + r);
    }
    else
        path.lineTo(x + w, y);

    if (bottomRight)
    {
        path.lineTo(x + w, y + h - r);
        path.quadraticTo(x + w, y + h, x + w - r, y + h);
    }
    else
        path.lineTo(x + w, y + h);

    if (bottomLeft)
    {
        path.lineTo(x + r, y + h);
        path.quadraticTo(x, y + h, x, y + h - r);
    }
    else
        path.lineTo(x, y + h);

    if (topLeft)
    {
        path.lineTo(x, y + r);
        path.quadraticTo(x, y, x + r, y);
    }
    else
        path.lineTo(x, y);

    path.closeSubPath();
}

class ColouredPopupItem final : public juce::PopupMenu::CustomComponent
{
public:
    ColouredPopupItem(juce::String textIn,
                      juce::Colour backgroundIn,
                      juce::Colour foregroundIn,
                      bool tickedIn,
                      bool enabledIn)
        : juce::PopupMenu::CustomComponent(true),
          text(std::move(textIn)),
          background(backgroundIn),
          foreground(foregroundIn),
          ticked(tickedIn),
          enabled(enabledIn)
    {
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        idealWidth = 230;
        idealHeight = 22;
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        juce::Colour bg = background;
        juce::Colour fg = foreground;
        if (!enabled)
        {
            bg = bg.withAlpha(0.40f);
            fg = fg.withAlpha(0.55f);
        }

        if (isItemHighlighted() && enabled)
            bg = bg.brighter(0.08f);

        g.setColour(bg);
        g.fillRoundedRectangle(area.reduced(1.0f, 0.5f), 4.0f);

        auto content = getLocalBounds().reduced(6, 0);
        auto tickArea = content.removeFromLeft(12).toFloat();
        if (ticked)
        {
            g.setColour(fg);
            g.fillEllipse(tickArea.withSizeKeepingCentre(6.0f, 6.0f));
        }

        g.setColour(fg);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText(text, content, juce::Justification::centredLeft, true);
    }

private:
    juce::String text;
    juce::Colour background;
    juce::Colour foreground;
    bool ticked = false;
    bool enabled = true;
};

void FlatComboBox::addItem(const juce::String& text, int itemId)
{
    items.add({ text, itemId });
}

void FlatComboBox::clear()
{
    items.clear();
    selectedIds.clear();
    selectedText.clear();
    selectedId = 0;
    repaint();
}

void FlatComboBox::setSelectedId(int id, juce::NotificationType notify)
{
    juce::Array<int> ids;
    if (containsItemId(id))
        ids.addIfNotAlreadyThere(id);
    setSelectedIds(ids, notify);
}

void FlatComboBox::setSelectedIds(const juce::Array<int>& ids, juce::NotificationType notify)
{
    juce::Array<int> canonicalIds;
    canonicalIds.ensureStorageAllocated(juce::jmin(ids.size(), items.size()));

    // Canonicalize in item-list order to keep display/persistence deterministic.
    for (const auto& item : items)
    {
        const int id = item.second;
        if (!ids.contains(id))
            continue;
        canonicalIds.addIfNotAlreadyThere(id);
        if (!multiSelectEnabled)
            break;
    }

    const bool changed = (canonicalIds != selectedIds);
    if (changed)
        selectedIds = canonicalIds;

    selectedId = selectedIds.isEmpty() ? 0 : selectedIds[0];
    refreshSelectedTextFromIds();
    repaint();

    if (changed && notify == juce::sendNotification && onChange)
        onChange();
}

void FlatComboBox::setMultiSelectEnabled(bool shouldEnable) noexcept
{
    if (multiSelectEnabled == shouldEnable)
        return;

    multiSelectEnabled = shouldEnable;

    if (!multiSelectEnabled && selectedIds.size() > 1)
    {
        juce::Array<int> firstOnly;
        firstOnly.add(selectedIds[0]);
        setSelectedIds(firstOnly, juce::dontSendNotification);
    }
}

void FlatComboBox::setItemColourPair(int itemId, juce::Colour background, juce::Colour foreground)
{
    itemColourPairs[itemId] = std::make_pair(background, foreground);
}

void FlatComboBox::clearItemColourPairs()
{
    itemColourPairs.clear();
}

void FlatComboBox::setMultiSelectionTextFormatter(std::function<juce::String(const juce::Array<int>&)> formatter)
{
    multiSelectionTextFormatter = std::move(formatter);
    refreshSelectedTextFromIds();
    repaint();
}

void FlatComboBox::addItemList(const juce::StringArray& texts, int firstItemId)
{
    for (int i = 0; i < texts.size(); ++i)
        addItem(texts[i], firstItemId + i);
}

bool FlatComboBox::containsItemId(int id) const noexcept
{
    for (const auto& item : items)
        if (item.second == id)
            return true;

    return false;
}

juce::String FlatComboBox::getLabelForItemId(int id) const
{
    for (const auto& item : items)
        if (item.second == id)
            return item.first;

    return {};
}

void FlatComboBox::refreshSelectedTextFromIds()
{
    if (selectedIds.isEmpty())
    {
        selectedText.clear();
        return;
    }

    juce::StringArray labels;
    labels.ensureStorageAllocated(selectedIds.size());
    for (const int id : selectedIds)
    {
        const auto label = getLabelForItemId(id);
        if (label.isNotEmpty())
            labels.add(label);
    }

    if (labels.isEmpty())
    {
        selectedText.clear();
        return;
    }

    if (multiSelectEnabled)
    {
        if (multiSelectionTextFormatter != nullptr)
        {
            const auto formattedText = multiSelectionTextFormatter(selectedIds);
            if (formattedText.isNotEmpty())
            {
                selectedText = formattedText;
                return;
            }
        }

        if (labels.size() == 1)
            selectedText = labels[0];
        else if (labels.size() == 2)
            selectedText = labels[0] + ", " + labels[1];
        else
            selectedText = labels[0] + " +" + juce::String(labels.size() - 1);

        return;
    }

    selectedText = labels[0];
}

void FlatComboBox::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const bool enabled = isEnabled();

    // Context style:
    // MainMenu utilise une palette differente pour garder la coherence
    // visuelle du header sans dupliquer le composant.
    bool isMainMenu = false;
    if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
        isMainMenu = (lf->getContextStyle() == PluginLookAndFeel::ComponentStyle::MainMenu);

    juce::Colour bodyColour;
    if (!enabled)
        bodyColour = PluginColours::disabled;
    else
        bodyColour = isMainMenu ? PluginColours::mainMenuPrimary
                                : PluginColours::primary;

    if (enabled && isMouseOver())
        bodyColour = bodyColour.brighter(0.05f);

    juce::Rectangle<float> textArea = bounds;

    if (showArrow)
    {
        // Layout: zone texte a gauche + zone fleche a droite.
        const float arrowWidth = 12.0f;
        auto arrowArea = bounds.removeFromRight(arrowWidth);
        textArea = bounds;

        // --- Corps principal (gauche)
        g.setColour(bodyColour);
        g.fillRect(bounds);

        // --- Pilule droite (flèche)
        juce::Path arrowBackground;
        const float radius = arrowArea.getHeight() * 0.5f;
        addSelectiveRoundedRect(arrowBackground, arrowArea, radius, false, true, false, true);

        g.setColour(!enabled ? PluginColours::disabled
                             : (isMainMenu ? PluginColours::mainMenuSurface
                                           : PluginColours::secondary));
        g.fillPath(arrowBackground);

        // Icone de fleche minimaliste pour rester legere au repaint.
        const float arrowSize = 4.0f;
        juce::Path path;
        path.startNewSubPath(arrowArea.getCentreX() - arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);
        path.lineTo(arrowArea.getCentreX(), arrowArea.getCentreY() + 2.0f);
        path.lineTo(arrowArea.getCentreX() + arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);

        g.setColour(!enabled ? PluginColours::onDisabled
                             : (isMainMenu ? PluginColours::onBackground
                                           : PluginColours::onSurface));
        g.strokePath(path, juce::PathStrokeType(1.5f));
    }
    else
    {
        juce::Path pillPath;
        const float radius = bounds.getHeight() * 0.5f;
        addSelectiveRoundedRect(pillPath, bounds, radius, true, true, true, true);

        g.setColour(bodyColour);
        g.fillPath(pillPath);
    }

    // --- Texte
    const int textPadding = 5;
    auto paddedTextArea = textArea.reduced(textPadding, 0);

    juce::Colour textColour = !enabled ? PluginColours::onDisabled
                                       : (isMainMenu ? PluginColours::onBackground
                                                     : PluginColours::onSurface);
    g.setColour(textColour);

    if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
        g.setFont(lf->getJostFont(12.0f, PluginLookAndFeel::JostWeight::Regular, false));
    else
        g.setFont(juce::Font(juce::FontOptions(12.0f)));

    g.drawText(selectedText.isEmpty() ? "-" : selectedText,
               paddedTextArea.toNearestInt(),
               showArrow ? juce::Justification::centredLeft
                         : juce::Justification::centred,
               true);
}

void FlatComboBox::showPopupMenu()
{
    juce::PopupMenu menu;
    juce::Array<int> selectableIds;
    selectableIds.ensureStorageAllocated(items.size());

    for (const auto& i : items)
    {
        const int id = i.second;
        const bool enabled = isItemEnabled ? isItemEnabled(id) : true;
        if (enabled)
            selectableIds.addIfNotAlreadyThere(id);
    }

    if (multiSelectEnabled && showMultiSelectActions && selectableIds.size() > 1)
    {
        bool hasSelected = false;
        bool allSelected = true;
        for (const int id : selectableIds)
        {
            const bool selected = selectedIds.contains(id);
            hasSelected = hasSelected || selected;
            allSelected = allSelected && selected;
        }

        menu.addSectionHeader("Selection");
        menu.addItem(kActionSelectAll, "Select all", true, allSelected);
        menu.addItem(kActionInvert, "Invert selection", true, false);
        menu.addItem(kActionClear, "Clear selection", hasSelected, false);
        menu.addSeparator();
    }

    for (const auto& i : items)
    {
        const int id = i.second;
        const bool enabled = isItemEnabled ? isItemEnabled(id) : true;
        const bool ticked =
            multiSelectEnabled ? selectedIds.contains(id)
                               : (id == selectedId);

        if (!enabled)
        {
            menu.addItem(id, i.first, false, ticked);
            continue;
        }

        const auto colourIt = itemColourPairs.find(id);
        if (colourIt != itemColourPairs.end())
        {
            menu.addCustomItem(id,
                               std::make_unique<ColouredPopupItem>(i.first,
                                                                   colourIt->second.first,
                                                                   colourIt->second.second,
                                                                   ticked,
                                                                   true));
            continue;
        }

        menu.addItem(id, i.first, enabled, ticked);
    }
    menu.setLookAndFeel(&getLookAndFeel());

    const int visibleId = selectedIds.isEmpty() ? selectedId : selectedIds[0];
    juce::Component::SafePointer<FlatComboBox> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withItemThatMustBeVisible(visibleId),
        [safeThis](int result)
        {
            if (safeThis == nullptr || result == 0)
                return;

            if (result == kActionSelectAll || result == kActionInvert || result == kActionClear)
            {
                juce::Array<int> selectableIds;
                selectableIds.ensureStorageAllocated(safeThis->items.size());
                for (const auto& item : safeThis->items)
                {
                    const int id = item.second;
                    const bool enabled = safeThis->isItemEnabled ? safeThis->isItemEnabled(id) : true;
                    if (enabled)
                        selectableIds.addIfNotAlreadyThere(id);
                }

                juce::Array<int> updated = safeThis->selectedIds;
                if (result == kActionSelectAll)
                {
                    for (const int id : selectableIds)
                        updated.addIfNotAlreadyThere(id);
                }
                else if (result == kActionInvert)
                {
                    for (const int id : selectableIds)
                    {
                        if (updated.contains(id))
                            updated.removeFirstMatchingValue(id);
                        else
                            updated.addIfNotAlreadyThere(id);
                    }
                }
                else if (result == kActionClear)
                {
                    updated.clear();
                }

                safeThis->setSelectedIds(updated, juce::sendNotification);
                return;
            }

            if (!safeThis->multiSelectEnabled)
            {
                safeThis->setSelectedId(result, juce::sendNotification);
                return;
            }

            auto updated = safeThis->selectedIds;
            if (updated.contains(result))
                updated.removeFirstMatchingValue(result);
            else
                updated.add(result);

            safeThis->setSelectedIds(updated, juce::sendNotification);

            if (safeThis->reopenPopupOnMultiSelect)
            {
                juce::MessageManager::callAsync([safeThis]()
                {
                    if (safeThis == nullptr || !safeThis->isShowing())
                        return;

                    if (safeThis->onClickBeforeOpen)
                        safeThis->onClickBeforeOpen();
                    safeThis->showPopupMenu();
                });
            }
        });
}

void FlatComboBox::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        // Clic droit: delegue au callback custom si defini.
        if (onRightClick)
            onRightClick(e);
        return;
    }

    // Hook optionnel pour preparer des items dynamiques juste avant ouverture.
    if (onClickBeforeOpen)
        onClickBeforeOpen();

    showPopupMenu();
}
