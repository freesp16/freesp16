#include "core/edit/StepCopyGesture.h"

namespace core::edit
{
void StepCopyGesture::setTarget(int scene, int pattern, int track, int shownPage) noexcept
{
    scene_ = scene & 15;
    pattern_ = pattern & 15;
    track_ = track < 0 || track > 15 ? kAllTracks : track;
    page_ = shownPage < 0 ? 0 : (shownPage > 3 ? 3 : shownPage);
}

void StepCopyGesture::copyPressed() noexcept
{
    copyHeld_ = true;
    pasteHeld_ = false;
    mask_ = 0;
}

void StepCopyGesture::copyReleased()
{
    if (! copyHeld_)
        return;
    copyHeld_ = false;
    if (mask_ != 0)
        editor_->copy(stepsRef(scene_, pattern_, track_, mask_));
    else
        editor_->copy(pageRef(scene_, pattern_, track_, page_)); // COPY alone: the shown bar
    mask_ = 0;
}

void StepCopyGesture::pastePressed() noexcept
{
    pasteHeld_ = true;
    copyHeld_ = false;
}

void StepCopyGesture::pasteReleased() noexcept
{
    pasteHeld_ = false;
}

bool StepCopyGesture::stepKey(int key)
{
    if (key < 1 || key > 16 || ! active())
        return false;
    const int step = page_ * 16 + key - 1;
    if (copyHeld_)
    {
        mask_ |= std::uint64_t {1} << step;
        return true;
    }
    const EditRef dst = stepsRef(scene_, pattern_, track_, std::uint64_t {1} << step);
    if (editor_->canPaste(dst))
        editor_->paste(dst);
    return true; // consumed even when the clipboard cannot paste here (PASTE held)
}
} // namespace core::edit
