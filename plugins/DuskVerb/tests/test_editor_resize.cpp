#include <JuceHeader.h>

#include "../src/PluginProcessor.h"
#include "SupportersOverlay.h"

#include <cmath>
#include <memory>
#include <utility>

namespace
{
juce::Component* findNamedComponent (juce::Component& parent, const juce::String& name)
{
    for (int i = 0; i < parent.getNumChildComponents(); ++i)
    {
        auto* child = parent.getChildComponent (i);
        if (child->getName() == name)
            return child;

        if (auto* match = findNamedComponent (*child, name))
            return match;
    }

    return nullptr;
}

juce::ResizableCornerComponent* findResizeHandle (juce::Component& parent)
{
    for (int i = 0; i < parent.getNumChildComponents(); ++i)
        if (auto* handle = dynamic_cast<juce::ResizableCornerComponent*> (parent.getChildComponent (i)))
            return handle;

    return nullptr;
}

bool supporterTextEntersFooter()
{
    constexpr int width = 600;
    constexpr int height = 300;
    constexpr int panelBottom = 270;
    constexpr int footerHeight = 75;

    SupportersOverlay overlay ("DuskVerb", "0.7.1");
    overlay.setActionLink ("Open crash log folder", [] {});
    overlay.setSize (width, height);

    juce::Image image (juce::Image::ARGB, width, height, true);
    juce::Graphics graphics (image);
    overlay.paint (graphics);

    // At this height the last patron row crosses the content/footer boundary
    // in the broken implementation. Supporter names are bright neutral text;
    // the divider/background in this narrow strip stay below this threshold.
    for (int y = panelBottom - footerHeight; y < panelBottom - footerHeight + 11; ++y)
    {
        for (int x = 180; x < width - 180; ++x)
        {
            const auto colour = image.getPixelAt (x, y);
            const int red = colour.getRed();
            const int green = colour.getGreen();
            const int blue = colour.getBlue();
            if (red >= 100 && std::abs (red - green) <= 2 && std::abs (green - blue) <= 2)
                return true;
        }
    }

    return false;
}

// The host resize path, which is not the in-editor grip path.
//
// JUCE's VST3 wrapper answers a host's IPlugView::checkSizeConstraint from the
// editor's constrainer (juce_audio_plugin_client_VST3.cpp), then sizes the
// editor to that answer. A host that keeps its own window at the size the user
// dragged, which REAPER on Linux does, is left with an editor bigger than the
// window and the face is clipped. This mirrors that arithmetic so the contract
// can be asserted without a host: whatever the host asks for, the editor must
// come back no larger, and the face must stay proportional inside it.
juce::Rectangle<int> hostAnswer (juce::ComponentBoundsConstrainer& constrainer,
                                 int requestedWidth, int requestedHeight)
{
    const auto minW = (float) constrainer.getMinimumWidth();
    const auto maxW = (float) constrainer.getMaximumWidth();
    const auto minH = (float) constrainer.getMinimumHeight();
    const auto maxH = (float) constrainer.getMaximumHeight();

    auto width  = juce::jlimit (minW, maxW, (float) requestedWidth);
    auto height = juce::jlimit (minH, maxH, (float) requestedHeight);

    const auto aspectRatio = (float) constrainer.getFixedAspectRatio();

    if (! juce::approximatelyEqual (aspectRatio, 0.0f))
    {
        if (width / height > aspectRatio)
        {
            width = height * aspectRatio;

            if (width > maxW || width < minW)
            {
                width = juce::jlimit (minW, maxW, width);
                height = width / aspectRatio;
            }
        }
        else
        {
            height = width / aspectRatio;

            if (height > maxH || height < minH)
            {
                height = juce::jlimit (minH, maxH, height);
                width = height * aspectRatio;
            }
        }
    }

    return { 0, 0, juce::roundToInt (width), juce::roundToInt (height) };
}

// Union of every visible descendant, in the editor's coordinate space. The
// resize grip is excluded: it is meant to sit in the corner of the window, not
// inside the letterboxed face.
juce::Rectangle<int> paintedBounds (juce::Component& component, juce::Component& root)
{
    juce::Rectangle<int> bounds;

    for (int i = 0; i < component.getNumChildComponents(); ++i)
    {
        auto* child = component.getChildComponent (i);

        if (child == nullptr || ! child->isVisible()
            || dynamic_cast<juce::ResizableCornerComponent*> (child) != nullptr)
            continue;

        bounds = bounds.getUnion (root.getLocalArea (child, child->getLocalBounds()));
        bounds = bounds.getUnion (paintedBounds (*child, root));
    }

    return bounds;
}
}

class DuskVerbEditorResizeTest final : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName() override { return "DuskVerbEditorResizeTest"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted (const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void systemRequestedQuit() override { quit(); }
    void shutdown() override {}
    void unhandledException (const std::exception*, const juce::String&, int) override {}

    void initialise (const juce::String&) override
    {
        DuskVerbProcessor processor;
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

        if (editor == nullptr)
        {
            ++failures;
            finish();
            return;
        }

        constexpr int baseWidth = 1400;
        constexpr int baseHeight = 760;
        constexpr int minWidth = 980;
        constexpr int minHeight = 532;

        auto* freezeButton = findNamedComponent (*editor, "freeze");
        auto* resizeHandle = findResizeHandle (*editor);
        const auto initialBounds = editor->getBounds();
        check (freezeButton != nullptr);
        check (resizeHandle != nullptr);

        editor->setBounds (0, 0, baseWidth, baseHeight);
        const int baseFreezeHeight = freezeButton != nullptr ? freezeButton->getHeight() : 0;

        editor->setBoundsConstrained ({ 0, 0, minWidth, minHeight });
        check (editor->getWidth() == minWidth && editor->getHeight() == minHeight);
        check (freezeButton != nullptr && freezeButton->getHeight() < baseFreezeHeight);
        check (resizeHandle != nullptr
               && resizeHandle->getBounds() == juce::Rectangle<int> (minWidth - 16, minHeight - 16, 16, 16));

        check (! supporterTextEntersFooter());

        // Issue #240: the host path. REAPER on Linux keeps its window at the
        // size the user dragged the frame to, so the editor must never come
        // back larger than the host asked for. Two geometries reproduced by
        // hand: 1057x474 lost 58 rows off the bottom, 625x340 was clipped on
        // both axes because it fell below the editor's comfort floor.
        const std::pair<int, int> hostSizes[] = { { 1057, 474 }, { 625, 340 }, { 1400, 500 }, { 900, 900 } };

        for (const auto& requested : hostSizes)
        {
            auto* constrainer = editor->getConstrainer();
            check (constrainer != nullptr);

            if (constrainer == nullptr)
                continue;

            // The host constrainer must not clamp: no minimum it can violate,
            // no aspect correction that grows either axis past the window.
            check (juce::approximatelyEqual (constrainer->getFixedAspectRatio(), 0.0));
            check (constrainer->getMinimumWidth() <= 1 && constrainer->getMinimumHeight() <= 1);

            editor->setBounds (hostAnswer (*constrainer, requested.first, requested.second));

            check (editor->getWidth() == requested.first && editor->getHeight() == requested.second);

            // The face is letterboxed inside those bounds, never clipped by them.
            const auto face = paintedBounds (*editor, *editor);
            check (face.getRight() <= editor->getWidth());
            check (face.getBottom() <= editor->getHeight());

            // The layout is fluid: widget sizes come from the shared scale factor
            // while the panels span the editor, so the face fills the window
            // rather than sitting in a letterbox. What matters for #240 is that
            // it lands inside the bounds the host gave, which the two checks
            // above assert, and that those bounds are the host's own size.
        }


        // DuskVerb persists its editor size on destruction. Restore the value
        // loaded at startup so this test never changes the user's preference.
        editor->setBounds (initialBounds);
        processor.editorBeingDeleted (editor.get());
        editor.reset();
        finish();
    }

private:
    int failures = 0;

    void check (bool condition)
    {
        if (! condition)
            ++failures;
    }

    void finish()
    {
        setApplicationReturnValue (failures == 0 ? 0 : 1);
        quit();
    }
};

START_JUCE_APPLICATION (DuskVerbEditorResizeTest)
