#pragma once
#include <JuceHeader.h>

class FlatComboBox : public juce::Component
{
public:
    FlatComboBox();

    void addItem(const juce::String& text, int itemId);
    void clear();

    void setSelectedId(int id, juce::NotificationType notify = juce::dontSendNotification);
    int getSelectedId() const noexcept { return selectedId; }
    juce::String getText() const noexcept { return selectedText; }

    /// Callback appelée quand la sélection change
    std::function<void()> onChange;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

    void resized() override {}

private:
    juce::Array<std::pair<juce::String, int>> items;
    juce::String selectedText;
    int selectedId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FlatComboBox)
};
