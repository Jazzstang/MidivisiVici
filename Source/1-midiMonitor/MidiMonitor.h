/**
 * @file MidiMonitor.h
 * @brief Virtualized MIDI monitor using a ListBox-backed ring buffer.
 *
 * Threading:
 * - `addMessage` can be called from non-UI contexts but only writes into a
 *   fixed ring and marks `displayDirty`.
 * - `flushPendingDisplay` must run on the message thread to refresh UI.
 * - Not RT-safe for direct UI calls.
 */
#pragma once

#include <JuceHeader.h>
#include <array>
#include "../PluginColours.h"

/**
 * @brief Displays formatted MIDI events with selection/copy/context actions.
 *
 * Pattern:
 * - Pattern: Producer/Consumer
 * - Problem solved: absorb event bursts without rebuilding a huge text model.
 * - Participants: producer (`addMessage`), ring storage, UI consumer
 *   (`flushPendingDisplay` + ListBox model).
 * - Flow: enqueue formatted line -> mark dirty -> periodic UI flush.
 * - Pitfalls: avoid forcing autoscroll when user is selecting rows.
 */
class MidiMonitor : public juce::Component,
                    private juce::ListBoxModel,
                    private juce::KeyListener
{
public:
    /** @brief Construct monitor and configure internal ListBox model. */
    MidiMonitor()
        : monitorFont(juce::FontOptions(getMonospacedFontName(), 10.0f, juce::Font::plain))
    {
        setOpaque(true);
        configureListBox();
        addAndMakeVisible(listBox);
    }

    ~MidiMonitor() override
    {
        listBox.removeKeyListener(this);
        listBox.setModel(nullptr);
    }

    /** @brief Clear ring buffer and reset ListBox state. */
    void clear()
    {
        writeIndex = 0;
        messageCount = 0;
        displayDirty = false;
        publishedRowCount = 0;
        lastAutoScrollTargetRow = -1;
        ringLineColours.fill(PluginColours::onSurface);
        listBox.deselectAllRows();
        listBox.updateContent();
        listBox.repaint();
    }

    /**
     * @brief Flush batched UI updates.
     *
     * Must be called on message thread (typically from shared UI scheduler).
     */
    void flushPendingDisplay()
    {
        if (!displayDirty)
            return;

        displayDirty = false;
        if (publishedRowCount != messageCount)
        {
            listBox.updateContent();
            publishedRowCount = messageCount;
        }
        else
        {
            listBox.repaint();
        }

        // Ne force pas le scroll si l'utilisateur est en train de sélectionner.
        if (messageCount > 0 &&
            listBox.getNumSelectedRows() == 0 &&
            lastAutoScrollTargetRow != (messageCount - 1))
        {
            listBox.scrollToEnsureRowIsOnscreen(messageCount - 1);
            lastAutoScrollTargetRow = messageCount - 1;
        }
    }

    /**
     * @brief Append one MIDI message to the ring buffer.
     *
     * This method formats textual output and updates fixed-size storage.
     */
    void addMessage(const juce::MidiMessage& m,
                    juce::Colour lineColour = PluginColours::onSurface)
    {
        const auto& noteNames = getCachedPaddedNoteNames();
        const auto& channelTexts = getCachedChannelTexts();
        const auto& velocityTexts = getCachedVelocityTexts();
        const auto& ccNumberTexts = getCachedCcNumberTexts();
        const auto& ccValueTexts = getCachedCcValueTexts();
        const auto& pcNumberTexts = getCachedPcNumberTexts();

        const juce::String* type = &getTypeUnknown();
        const juce::String* channel = &getGlobalChannelText();
        juce::String message;
        juce::String value;

        // === Canal MIDI ou global ===
        const juce::uint8 status = m.getRawDataSize() > 0 ? m.getRawData()[0] : 0;

        if (m.isSysEx() || m.isMidiClock() || m.isSongPositionPointer() || m.isQuarterFrame() || status == 0xF3)
        {
            channel = &getGlobalChannelText();
        }
        else
        {
            channel = &channelTexts[(size_t) juce::jlimit(1, 16, m.getChannel()) - 1];
        }

        // === Traitement des types ===
        if (m.isSysEx())
        {
            const auto* data = m.getSysExData();
            const int len = m.getSysExDataSize();
            juce::String text = juce::String::fromUTF8(reinterpret_cast<const char*>(data), len);
            type = &getTypeSysEx();
            message = "[DEBUG] " + text;
        }
        else if (m.isNoteOn() && m.getVelocity() > 0)
        {
            type = &getTypeNoteOn();
            const int note = juce::jlimit(0, 127, m.getNoteNumber());
            const int velocityInt = static_cast<int>(m.getVelocity());
            message = noteNames[(size_t) note];
            value = velocityTexts[(size_t) juce::jlimit(0, 127, velocityInt)];
        }
        else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0))
        {
            type = &getTypeNoteOff();
            message = noteNames[(size_t) juce::jlimit(0, 127, m.getNoteNumber())];
        }
        else if (m.isController())
        {
            type = &getTypeCc();
            const int controller = juce::jlimit(0, 127, m.getControllerNumber());
            message = ccNumberTexts[(size_t) controller];
            value = ccValueTexts[(size_t) juce::jlimit(0, 127, m.getControllerValue())];
        }
        else if (m.isProgramChange())
        {
            type = &getTypeProgramChange();
            message = pcNumberTexts[(size_t) juce::jlimit(0, 127, m.getProgramChangeNumber())];
        }
        else if (m.isPitchWheel())
        {
            type = &getTypePitchBend();
            message = juce::String(m.getPitchWheelValue());
        }
        else if (m.isAftertouch())
        {
            type = &getTypeAftertouch();
            message = juce::String(m.getAfterTouchValue());
        }
        else if (m.isChannelPressure())
        {
            type = &getTypeChannelPressure();
            message = juce::String(m.getChannelPressureValue());
        }
        else if (m.isMidiClock())
        {
            type = &getTypeClock();
            message = "Clock";
        }
        else if (m.isQuarterFrame())
        {
            type = &getTypeClock();
            message = "Quarter Frame";
        }
        else if (m.isSongPositionPointer())
        {
            type = &getTypeClock();
            message = "Song Position Pointer";
        }
        else if (m.getRawDataSize() == 2 && m.getRawData()[0] == 0xF3) // Song Select
        {
            type = &getTypeClock();
            message = "Song Select";
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff() || m.isActiveSense()
                 || (m.getRawDataSize() >= 1 && m.getRawData()[0] == 0xFF)) // System Reset
        {
            type = &getTypeEvent();
            message = m.getDescription();
        }
        else
        {
            type = &getTypeUnknown();
            message = m.getDescription();
        }

        juce::String formatted;
        formatted.preallocateBytes(96);
        formatted << *type << " | " << *channel << " | " << message;
        if (value.isNotEmpty())
            formatted << " | " << value;

        ringMessages[(size_t) writeIndex] = formatted;
        ringLineColours[(size_t) writeIndex] = lineColour;
        writeIndex = (writeIndex + 1) % maxMessages;
        messageCount = juce::jmin(maxMessages, messageCount + 1);

        displayDirty = true;
    }

    void resized() override
    {
        listBox.setBounds(getLocalBounds().reduced(4));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(PluginColours::surface);
    }

private:
    //--------------------------------------------------------------------------
    // Static text caches
    //--------------------------------------------------------------------------
    // All formatting strings are cached once to:
    // - reduce per-message allocations,
    // - keep monitor rendering stable under MIDI bursts.
    //--------------------------------------------------------------------------
    static const std::array<juce::String, 128>& getCachedPaddedNoteNames()
    {
        static const std::array<juce::String, 128> cache = []()
        {
            std::array<juce::String, 128> values {};
            for (int note = 0; note < 128; ++note)
            {
                const juce::String raw = juce::MidiMessage::getMidiNoteName(note, true, true, 4);
                const juce::String normalized = (raw.length() == 2)
                                                    ? (raw.substring(0, 1) + " " + raw.substring(1))
                                                    : raw;
                values[(size_t) note] = normalized.paddedRight(' ', 4);
            }
            return values;
        }();

        return cache;
    }

    static const std::array<juce::String, 16>& getCachedChannelTexts()
    {
        static const std::array<juce::String, 16> cache = []()
        {
            std::array<juce::String, 16> values {};
            for (int ch = 1; ch <= 16; ++ch)
                values[(size_t) (ch - 1)] = "Ch. " + juce::String(ch).paddedLeft(' ', 2);
            return values;
        }();
        return cache;
    }

    static const std::array<juce::String, 128>& getCachedVelocityTexts()
    {
        static const std::array<juce::String, 128> cache = []()
        {
            std::array<juce::String, 128> values {};
            for (int vel = 0; vel < 128; ++vel)
                values[(size_t) vel] = "Velocity : " + juce::String(vel).paddedLeft(' ', 4);
            return values;
        }();
        return cache;
    }

    static const std::array<juce::String, 128>& getCachedCcNumberTexts()
    {
        static const std::array<juce::String, 128> cache = []()
        {
            std::array<juce::String, 128> values {};
            for (int cc = 0; cc < 128; ++cc)
                values[(size_t) cc] = "CC#" + juce::String(cc).paddedLeft(' ', 4);
            return values;
        }();
        return cache;
    }

    static const std::array<juce::String, 128>& getCachedCcValueTexts()
    {
        static const std::array<juce::String, 128> cache = []()
        {
            std::array<juce::String, 128> values {};
            for (int value = 0; value < 128; ++value)
                values[(size_t) value] = "Value    : " + juce::String(value).paddedLeft(' ', 4);
            return values;
        }();
        return cache;
    }

    static const std::array<juce::String, 128>& getCachedPcNumberTexts()
    {
        static const std::array<juce::String, 128> cache = []()
        {
            std::array<juce::String, 128> values {};
            for (int pc = 0; pc < 128; ++pc)
                values[(size_t) pc] = "PC#" + juce::String(pc).paddedLeft(' ', 4);
            return values;
        }();
        return cache;
    }

    static const juce::String& getGlobalChannelText()
    {
        static const juce::String text = "global";
        return text;
    }

    static const juce::String& getTypeSysEx()
    {
        static const juce::String text = "SysEx     ";
        return text;
    }

    static const juce::String& getTypeNoteOn()
    {
        static const juce::String text = "Note On   ";
        return text;
    }

    static const juce::String& getTypeNoteOff()
    {
        static const juce::String text = "Note Off  ";
        return text;
    }

    static const juce::String& getTypeCc()
    {
        static const juce::String text = "CC        ";
        return text;
    }

    static const juce::String& getTypeProgramChange()
    {
        static const juce::String text = "Program Change";
        return text;
    }

    static const juce::String& getTypePitchBend()
    {
        static const juce::String text = "Pitch Bend";
        return text;
    }

    static const juce::String& getTypeAftertouch()
    {
        static const juce::String text = "Aftertouch";
        return text;
    }

    static const juce::String& getTypeChannelPressure()
    {
        static const juce::String text = "Channel Pressure";
        return text;
    }

    static const juce::String& getTypeClock()
    {
        static const juce::String text = "Clock     ";
        return text;
    }

    static const juce::String& getTypeEvent()
    {
        static const juce::String text = "Event     ";
        return text;
    }

    static const juce::String& getTypeUnknown()
    {
        static const juce::String text = "Unknown   ";
        return text;
    }

    static juce::String getMonospacedFontName()
    {
        const juce::StringArray preferredFonts = { "Menlo", "SF Mono", "Monaco", "Courier New" };
        const auto availableFonts = juce::Font::findAllTypefaceNames();

        for (const auto& fontName : preferredFonts)
        {
            if (availableFonts.contains(fontName))
                return fontName;
        }

        return juce::Font::getDefaultMonospacedFontName();
    }

    void configureListBox()
    {
        listBox.setModel(this);
        listBox.setMultipleSelectionEnabled(true);
        listBox.setRowHeight(14);
        listBox.setOutlineThickness(0);
        listBox.setWantsKeyboardFocus(true);
        listBox.setMouseMoveSelectsRows(false);
        listBox.addKeyListener(this);

        listBox.setColour(juce::ListBox::backgroundColourId, PluginColours::surface);
        listBox.setColour(juce::ListBox::outlineColourId, PluginColours::surface);

        if (auto* viewport = listBox.getViewport())
            viewport->setScrollBarsShown(true, false);
    }

    //--------------------------------------------------------------------------
    // Ring-buffer row mapping
    //--------------------------------------------------------------------------
    // ListBox rows are chronological [oldest..newest], while storage is a cyclic
    // ring [0..maxMessages-1]. This helper converts display row -> ring slot.
    //--------------------------------------------------------------------------
    int getRingIndexForDisplayRow(int row) const noexcept
    {
        if (!juce::isPositiveAndBelow(row, messageCount))
            return -1;

        const int first = (writeIndex - messageCount + maxMessages) % maxMessages;
        return (first + row) % maxMessages;
    }

    const juce::String& getDisplayRowTextRef(int row) const
    {
        static const juce::String empty;
        const int idx = getRingIndexForDisplayRow(row);
        if (idx < 0)
            return empty;

        return ringMessages[(size_t) idx];
    }

    juce::Colour getDisplayRowColour(int row) const noexcept
    {
        const int idx = getRingIndexForDisplayRow(row);
        if (idx < 0)
            return PluginColours::onSurface;

        return ringLineColours[(size_t) idx];
    }

    juce::String getDisplayRowText(int row) const
    {
        return getDisplayRowTextRef(row);
    }

    //--------------------------------------------------------------------------
    // Clipboard helpers
    //--------------------------------------------------------------------------
    // Copy operations intentionally use displayed rows so user selection order
    // matches what is visible in the monitor.
    //--------------------------------------------------------------------------
    void copySelectedRowsToClipboard() const
    {
        const int selectedCount = listBox.getNumSelectedRows();
        if (selectedCount <= 0)
            return;

        juce::StringArray rows;
        rows.ensureStorageAllocated(selectedCount);

        for (int i = 0; i < selectedCount; ++i)
            rows.add(getDisplayRowTextRef(listBox.getSelectedRow(i)));

        juce::SystemClipboard::copyTextToClipboard(rows.joinIntoString("\n"));
    }

    void copyAllRowsToClipboard() const
    {
        if (messageCount <= 0)
            return;

        juce::StringArray rows;
        rows.ensureStorageAllocated(messageCount);

        for (int i = 0; i < messageCount; ++i)
            rows.add(getDisplayRowTextRef(i));

        juce::SystemClipboard::copyTextToClipboard(rows.joinIntoString("\n"));
    }

    // Context menu is async to avoid modal loop reentrancy in host editors.
    void showContextMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Copy", listBox.getNumSelectedRows() > 0);
        menu.addItem(2, "Copy All", messageCount > 0);
        menu.addSeparator();
        menu.addItem(3, "Clear", messageCount > 0);

        juce::Component::SafePointer<MidiMonitor> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&listBox),
                           [safeThis](int picked)
                           {
                               if (safeThis == nullptr)
                                   return;

                               safeThis->handleContextMenuResult(picked);
                           });
    }

    void handleContextMenuResult(int picked)
    {
        if (picked == 1)
            copySelectedRowsToClipboard();
        else if (picked == 2)
            copyAllRowsToClipboard();
        else if (picked == 3)
            clear();
    }

    //==========================================================================
    // ListBoxModel
    //==========================================================================
    int getNumRows() override
    {
        return messageCount;
    }

    void paintListBoxItem(int rowNumber,
                          juce::Graphics& g,
                          int width,
                          int height,
                          bool rowIsSelected) override
    {
        g.fillAll(rowIsSelected ? PluginColours::primary : PluginColours::surface);
        g.setColour(rowIsSelected ? PluginColours::onPrimary
                                  : getDisplayRowColour(rowNumber));
        g.setFont(monitorFont);

        const auto& text = getDisplayRowTextRef(rowNumber);
        g.drawText(text, juce::Rectangle<int>(0, 0, width, height).reduced(2, 0), juce::Justification::centredLeft, false);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& event) override
    {
        if (!event.mods.isPopupMenu())
            return;

        if (juce::isPositiveAndBelow(row, messageCount) && !listBox.isRowSelected(row))
            listBox.selectRow(row, true, false);

        showContextMenu();
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (!juce::isPositiveAndBelow(row, messageCount))
            return;

        listBox.selectRow(row);
        copySelectedRowsToClipboard();
    }

    void backgroundClicked(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            showContextMenu();
    }

    //==========================================================================
    // KeyListener
    //==========================================================================
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        const bool cmdOrCtrl = key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown();
        if (!cmdOrCtrl)
            return false;

        const auto c = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());

        if (c == 'c')
        {
            copySelectedRowsToClipboard();
            return true;
        }

        if (c == 'a')
        {
            if (messageCount > 0)
                listBox.selectRangeOfRows(0, messageCount - 1);
            return true;
        }

        return false;
    }

    //--------------------------------------------------------------------------
    // Storage and render model
    //--------------------------------------------------------------------------
    // maxMessages:
    // - hard cap for memory stability in long sessions.
    // - old rows are overwritten FIFO-style when the ring is full.
    //--------------------------------------------------------------------------
    static constexpr int maxMessages = 200;

    juce::ListBox listBox { "MidiMonitorList", this };
    std::array<juce::String, maxMessages> ringMessages {};
    std::array<juce::Colour, maxMessages> ringLineColours {};
    int writeIndex = 0;
    int messageCount = 0;
    bool displayDirty = false;
    int publishedRowCount = 0;
    int lastAutoScrollTargetRow = -1;
    juce::Font monitorFont;
};
