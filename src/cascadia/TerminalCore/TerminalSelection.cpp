// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"

using namespace Microsoft::Terminal::Core;

// Method Description:
// - Helper to determine the selected region of the buffer. Used for rendering.
// Return Value:
// - A vector of rectangles representing the regions to select, line by line. They are absolute coordinates relative to the buffer origin.
std::vector<SMALL_RECT> Terminal::_GetSelectionRects() const
{
    std::vector<SMALL_RECT> selectionArea;

    if (!_selectionActive)
    {
        return selectionArea;
    }

    // Add anchor offset here to update properly on new buffer output
    SHORT temp1, temp2;
    THROW_IF_FAILED(ShortAdd(_selectionAnchor.Y, _selectionAnchor_YOffset, &temp1));
    THROW_IF_FAILED(ShortAdd(_endSelectionPosition.Y, _endSelectionPosition_YOffset, &temp2));

    // create these new anchors for comparison and rendering
    const COORD selectionAnchorWithOffset = { _selectionAnchor.X, temp1 };
    const COORD endSelectionPositionWithOffset = { _endSelectionPosition.X, temp2 };

    // NOTE: (0,0) is top-left so vertical comparison is inverted
    const COORD &higherCoord = (selectionAnchorWithOffset.Y <= endSelectionPositionWithOffset.Y) ? selectionAnchorWithOffset : endSelectionPositionWithOffset;
    const COORD &lowerCoord = (selectionAnchorWithOffset.Y > endSelectionPositionWithOffset.Y) ? selectionAnchorWithOffset : endSelectionPositionWithOffset;

    selectionArea.reserve(lowerCoord.Y - higherCoord.Y + 1);
    for (auto row = higherCoord.Y; row <= lowerCoord.Y; row++)
    {
        SMALL_RECT selectionRow;

        selectionRow.Top = row;
        selectionRow.Bottom = row;

        if (_boxSelection || higherCoord.Y == lowerCoord.Y)
        {
            selectionRow.Left = std::min(higherCoord.X, lowerCoord.X);
            selectionRow.Right = std::max(higherCoord.X, lowerCoord.X);
        }
        else
        {
            selectionRow.Left = (row == higherCoord.Y) ? higherCoord.X : 0;
            selectionRow.Right = (row == lowerCoord.Y) ? lowerCoord.X : _buffer->GetSize().RightInclusive();
        }

        _ExpandWideGlyphSelection_Left(selectionRow.Left, row);
        _ExpandWideGlyphSelection_Right(selectionRow.Right, row);

        selectionArea.emplace_back(selectionRow);
    }
    return selectionArea;
}

// Method Description:
// - Expands the selection left-wards to cover a wide glyph, if necessary
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
// Return Value:
// - updates "position" to the proper value, if necessary
void Terminal::_ExpandWideGlyphSelection_Left(SHORT& x_pos, const SHORT y_pos) const noexcept
{
    COORD position{x_pos, y_pos};
    const auto attr = _buffer->GetCellDataAt(position)->DbcsAttr();
    if (attr.IsTrailing())
    {
        // try to move off by highlighting the lead half too.
        bool fSuccess = _mutableViewport.DecrementInBounds(position);

        // if that fails, move off to the next character
        if (!fSuccess)
        {
            _mutableViewport.IncrementInBounds(position);
        }
    }
    x_pos = position.X;
}

// Method Description:
// - Expands the selection left-wards to cover a wide glyph, if necessary
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
// Return Value:
// - updates "position" to the proper value, if necessary
void Terminal::_ExpandWideGlyphSelection_Right(SHORT& x_pos, const SHORT y_pos) const noexcept
{
    COORD position{ x_pos, y_pos };
    const auto attr = _buffer->GetCellDataAt(position)->DbcsAttr();
    if (attr.IsLeading())
    {
        // try to move off by highlighting the lead half too.
        bool fSuccess = _mutableViewport.IncrementInBounds(position);

        // if that fails, move off to the next character
        if (!fSuccess)
        {
            _mutableViewport.DecrementInBounds(position);
        }
    }
    x_pos = position.X;
}

// Method Description:
// - Checks if selection is active
// Return Value:
// - bool representing if selection is active. Used to decide copy/paste on right click
const bool Terminal::IsSelectionActive() const noexcept
{
    return _selectionActive;
}

// Method Description:
// - Record the position of the beginning of a selection
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
void Terminal::SetSelectionAnchor(const COORD position)
{
    _selectionAnchor = position;

    // include _scrollOffset here to ensure this maps to the right spot of the original viewport
    THROW_IF_FAILED(ShortSub(_selectionAnchor.Y, gsl::narrow<SHORT>(_scrollOffset), &_selectionAnchor.Y));

    // copy value of ViewStartIndex to support scrolling
    // and update on new buffer output (used in _GetSelectionRects())
    _selectionAnchor_YOffset = gsl::narrow<SHORT>(_ViewStartIndex());

    _selectionActive = true;
    SetEndSelectionPosition(position);
}

// Method Description:
// - Record the position of the end of a selection
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
void Terminal::SetEndSelectionPosition(const COORD position)
{
    _endSelectionPosition = position;

    // include _scrollOffset here to ensure this maps to the right spot of the original viewport
    THROW_IF_FAILED(ShortSub(_endSelectionPosition.Y, gsl::narrow<SHORT>(_scrollOffset), &_endSelectionPosition.Y));

    // copy value of ViewStartIndex to support scrolling
    // and update on new buffer output (used in _GetSelectionRects())
    _endSelectionPosition_YOffset = gsl::narrow<SHORT>(_ViewStartIndex());
}

// Method Description:
// - enable/disable box selection (ALT + selection)
// Arguments:
// - isEnabled: new value for _boxSelection
void Terminal::SetBoxSelection(const bool isEnabled) noexcept
{
    _boxSelection = isEnabled;
}

// Method Description:
// - clear selection data and disable rendering it
void Terminal::ClearSelection() noexcept
{
    _selectionActive = false;
    _selectionAnchor = { 0, 0 };
    _endSelectionPosition = { 0, 0 };
    _selectionAnchor_YOffset = 0;
    _endSelectionPosition_YOffset = 0;
}

// Method Description:
// - get wstring text from highlighted portion of text buffer
// Return Value:
// - wstring text from buffer. If extended to multiple lines, each line is separated by \r\n
const std::wstring Terminal::RetrieveSelectedTextFromBuffer(bool trimTrailingWhitespace) const
{
    std::function<COLORREF(TextAttribute&)> GetForegroundColor = std::bind(&Terminal::GetForegroundColor, this, std::placeholders::_1);
    std::function<COLORREF(TextAttribute&)> GetBackgroundColor = std::bind(&Terminal::GetBackgroundColor, this, std::placeholders::_1);

    auto data = _buffer->GetTextForClipboard(!_boxSelection,
                                             trimTrailingWhitespace,
                                             _GetSelectionRects(),
                                             GetForegroundColor,
                                             GetBackgroundColor);

    std::wstring result;
    for (const auto& text : data.text)
    {
        result += text;
    }

    return result;
}