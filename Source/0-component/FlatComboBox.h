/**
 * @file FlatComboBox.h
 * @brief Lightweight flat combo component with custom popup behavior.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include <JuceHeader.h>
#include <map>

/**
 * @brief Custom combo-like control with explicit callbacks and item filtering.
 */
class FlatComboBox : public juce::Component
{
public:
    FlatComboBox();

    /** @brief Add one item with explicit ID. */
    void addItem(const juce::String& text, int itemId);

    /** @brief Add a list of items starting at `firstItemId`. */
    void addItemList(const juce::StringArray& texts, int firstItemId = 1);

    /** @brief Remove all items and reset selection state. */
    void clear();

    /** @brief Set selected item by ID. */
    void setSelectedId(int id, juce::NotificationType notify = juce::dontSendNotification);
    /** @brief Set selected item IDs (multi-select mode). */
    void setSelectedIds(const juce::Array<int>& ids, juce::NotificationType notify = juce::dontSendNotification);
    /** @brief Return current selected item ID. */
    int getSelectedId() const noexcept { return selectedId; }
    /** @brief Return selected IDs. */
    juce::Array<int> getSelectedIds() const noexcept { return selectedIds; }
    /** @brief Return current selected text label. */
    juce::String getText() const noexcept { return selectedText; }
    /** @brief Enable/disable multi-select popup behavior. */
    void setMultiSelectEnabled(bool shouldEnable) noexcept;
    /** @brief Return true when combo is in multi-select mode. */
    bool isMultiSelectEnabled() const noexcept { return multiSelectEnabled; }
    /** @brief Show/hide right-side arrow icon. */
    void setShowArrow(bool shouldShow) noexcept { showArrow = shouldShow; repaint(); }
    /** @brief Return true if arrow icon is visible. */
    bool getShowArrow() const noexcept { return showArrow; }
    /** @brief Set custom popup item colours for one item id. */
    void setItemColourPair(int itemId, juce::Colour background, juce::Colour foreground);
    /** @brief Remove all custom popup item colours. */
    void clearItemColourPairs();
    /**
     * @brief In multi-select mode, automatically reopen popup after each toggle.
     * Useful for fast "checkbox-like" destination selection.
     */
    void setReopenPopupOnMultiSelect(bool shouldReopen) noexcept { reopenPopupOnMultiSelect = shouldReopen; }
    /** @brief Return auto-reopen flag for multi-select mode. */
    bool getReopenPopupOnMultiSelect() const noexcept { return reopenPopupOnMultiSelect; }
    /**
     * @brief Show/hide quick actions (select all / invert / clear) in multi-select popup.
     */
    void setShowMultiSelectActions(bool shouldShow) noexcept { showMultiSelectActions = shouldShow; }
    /** @brief Return true when quick actions are visible in multi-select popup. */
    bool getShowMultiSelectActions() const noexcept { return showMultiSelectActions; }
    /**
     * @brief Optional text formatter for multi selection display text.
     * Input list is already canonicalized by current item list.
     */
    void setMultiSelectionTextFormatter(std::function<juce::String(const juce::Array<int>&)> formatter);

    /** @brief Fired when selection changes. */
    std::function<void()> onChange;
    
    /**
     * @brief Optional item filter evaluated when opening popup.
     * @return True when an item is selectable.
     */
    std::function<bool(int itemId)> isItemEnabled;
    
    /** @brief Fired before opening popup list. */
    std::function<void()> onClickBeforeOpen;
    
    /**
     * @brief Right-click handler.
     *
     * If set, right click does not open the popup list.
     */
    std::function<void(const juce::MouseEvent&)> onRightClick;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void resized() override {}

private:
    void showPopupMenu();
    void refreshSelectedTextFromIds();
    bool containsItemId(int id) const noexcept;
    juce::String getLabelForItemId(int id) const;

    juce::Array<std::pair<juce::String, int>> items;
    juce::Array<int> selectedIds;
    juce::String selectedText;
    int selectedId = 0;
    bool showArrow = true;
    bool multiSelectEnabled = false;
    bool reopenPopupOnMultiSelect = true;
    bool showMultiSelectActions = true;
    std::map<int, std::pair<juce::Colour, juce::Colour>> itemColourPairs;
    std::function<juce::String(const juce::Array<int>&)> multiSelectionTextFormatter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FlatComboBox)
};
