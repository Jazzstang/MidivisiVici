#include "FlatComboBox.h"
#include "PluginLookAndFeel.h"
#include "PluginColours.h"

FlatComboBox::FlatComboBox()
{
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);
}

void FlatComboBox::addItem(const juce::String& text, int itemId)
{
    items.add({ text, itemId });
}

void FlatComboBox::clear()
{
    items.clear();
    selectedText.clear();
    selectedId = 0;
    repaint();
}

void FlatComboBox::setSelectedId(int id, juce::NotificationType notify)
{
    for (auto& i : items)
    {
        if (i.second == id)
        {
            selectedText = i.first;
            selectedId = id;
            repaint();
            if (notify == juce::sendNotification && onChange)
                onChange();
            return;
        }
    }
    // si l'id n'existe pas
    selectedText.clear();
    selectedId = 0;
    repaint();
}

void FlatComboBox::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // --- Fond principal ---
    g.setColour(isMouseOver() ? PluginColours::surface.brighter(0.05f)
                              : PluginColours::surface);
    g.fillRect(bounds);

    // --- Zone flèche ---
    const float arrowWidth = 12.0f;
    auto arrowArea = bounds.removeFromRight(arrowWidth);
    g.setColour(PluginColours::surface.darker(0.1f));
    g.fillRect(arrowArea);

    // --- Texte (padding gauche) ---
    const int textPadding = 6;
    auto textArea = bounds.reduced(textPadding, 0);

    g.setColour(PluginColours::onSurface);

    if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
        g.setFont(lf->getJostFont(14.0f, PluginLookAndFeel::JostWeight::Regular, false));
    else
        g.setFont(juce::Font(14.0f));

    g.drawText(selectedText.isEmpty() ? "-" : selectedText, textArea.toNearestInt(),
               juce::Justification::centredLeft, true);

    // --- Flèche ---
    const float arrowSize = 4.0f;
    juce::Path path;
    path.startNewSubPath(arrowArea.getCentreX() - arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);
    path.lineTo(arrowArea.getCentreX(), arrowArea.getCentreY() + 2.0f);
    path.lineTo(arrowArea.getCentreX() + arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);
    g.setColour(PluginColours::onSurface);
    g.strokePath(path, juce::PathStrokeType(1.5f));
}

void FlatComboBox::mouseDown(const juce::MouseEvent&)
{
    juce::PopupMenu menu;
    for (auto& i : items)
        menu.addItem(i.second, i.first, true, i.second == selectedId);

    // Appliquer le LookAndFeel courant (important pour éviter la fermeture immédiate)
    menu.setLookAndFeel(&getLookAndFeel());

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withItemThatMustBeVisible(selectedId),
        [this](int result)
        {
            if (result > 0)
                setSelectedId(result, juce::sendNotification);
        });
}