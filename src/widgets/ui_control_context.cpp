// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/control_context.hpp>
#include <portals/shortcut.hpp>
#include <portals/style.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace portals::ui {
namespace {
using Code = ControlContextErrorCode;
using Kind = ControlPropertyKind;
using Mask = detail::ControlPropertyPresence;
constexpr std::size_t PAINT_LIMIT_PROPERTY = 39;
constexpr std::size_t WHEEL_STEP_PROPERTY = 44;
constexpr std::size_t MINIMUM_THUMB_PROPERTY = 47;
constexpr std::size_t ROW_HEIGHT_PROPERTY = 69;
constexpr std::size_t STATUS_ROWS_PROPERTY = 81;
constexpr std::size_t DIALOG_WIDTH_PROPERTY = 94;
constexpr std::size_t DIALOG_HEIGHT_PROPERTY = 95;
constexpr std::size_t NOTIFICATION_WIDTH_PROPERTY = 118;
constexpr std::size_t SELECTOR_SHORTCUT_PROPERTY = 142;
constexpr std::array<std::string_view, 3> STATE_NAMES{"UNCHECKED", "CHECKED", "MIXED"};
constexpr std::array<std::string_view, 2> CYCLE_NAMES{"TWO_STATE", "THREE_STATE"};
constexpr std::array<std::string_view, 3> MODE_NAMES{"IMMEDIATE", "ON_COMPLETION", "EXPLICIT"};
constexpr std::array<std::string_view, 2> CHANGE_NAMES{"RECORD", "RESET_BASELINE"};
constexpr std::array<std::string_view, 3> DISPLAY_NAMES{"NORMAL", "PASSWORD", "NO_ECHO"};
constexpr std::array<std::string_view, 2> FLOW_NAMES{"ROW_MAJOR", "COLUMN_MAJOR"};
constexpr std::array<std::string_view, 4> ALIGN_NAMES{"START", "CENTER", "END", "STRETCH"};
constexpr std::array<std::string_view, 1> OVERFLOW_NAMES{"CLIP"};
constexpr std::array<std::string_view, 2> CHROME_NAMES{"BRACKETS", "NONE"};
constexpr std::array<std::string_view, 2> GLYPH_NAMES{"UNICODE", "ASCII"};
constexpr std::array<std::string_view, 3> BAR_POLICY_NAMES{"NEVER", "AUTO", "ALWAYS"};
constexpr std::array<std::string_view, 3> PLACEMENT_NAMES{"RESERVED", "OVERLAY", "ON_FRAME"};
constexpr std::array<std::string_view, 2> CONTENT_POLICY_NAMES{"KEEP_BODY", "REPLACE"};
constexpr std::array<std::string_view, 4> TOAST_PLACEMENT_NAMES{"BOTTOM_END", "TOP_END", "BOTTOM_START", "TOP_START"};
constexpr std::array<std::string_view, 2> TOAST_OVERFLOW_NAMES{"DROP_OLDEST_PENDING", "REJECT_NEW"};
constexpr std::array<std::string_view, 5> MESSAGE_CHOICE_NAMES{"ACKNOWLEDGE", "CONFIRM", "SAVE", "DISCARD", "CANCEL"};
constexpr std::array<std::string_view, 2> DIALOG_MODE_NAMES{"MODAL", "NON_MODAL"};
constexpr std::array<std::string_view, 3> DIALOG_PLACEMENT_NAMES{"CENTER", "BELOW_START", "ABOVE_START"};
constexpr std::array<std::string_view, 2> CHART_MODE_NAMES{"LINE", "CANDLES"};
constexpr std::array<std::string_view, 2> CHART_GLYPH_NAMES{"ASCII", "BOX_DRAWING"};
constexpr std::array<std::string_view, 2> AXIS_NAMES{"HORIZONTAL", "VERTICAL"};
constexpr std::array<std::string_view, 1> PLACEHOLDER_ALIASES{"placeHolder"};
constexpr std::array<ControlPropertyDescriptor, detail::CONTROL_PROPERTY_COUNT> PROPERTIES{
  {{"text", Kind::TEXT, 1, {}},
   {"width", Kind::SIGNED_INTEGER, 1, {}},
   {"height", Kind::SIGNED_INTEGER, 1, {}},
   {"flex", Kind::UNSIGNED_INTEGER, 1, {}},
   {"gap", Kind::SIGNED_INTEGER, 1, {}},
   {"state", Kind::ENUMERATION, 1, STATE_NAMES},
   {"cycle", Kind::ENUMERATION, 1, CYCLE_NAMES},
   {"editMode", Kind::ENUMERATION, 1, MODE_NAMES},
   {"readOnly", Kind::BOOLEAN, 1, {}},
   {"programmaticChange", Kind::ENUMERATION, 1, CHANGE_NAMES},
   {"displayMode", Kind::ENUMERATION, 1, DISPLAY_NAMES},
   {"placeholder", Kind::TEXT, 1, {}, PLACEHOLDER_ALIASES},
   {"history.enabled", Kind::BOOLEAN, 2, {}},
   {"history.maxOperations", Kind::UNSIGNED_INTEGER, 2, {}},
   {"history.maxBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"history.groupTyping", Kind::BOOLEAN, 2, {}},
   {"history.groupDeletion", Kind::BOOLEAN, 2, {}},
   {"columns", Kind::UNSIGNED_INTEGER, 1, {}},
   {"rows", Kind::UNSIGNED_INTEGER, 1, {}},
   {"flow", Kind::ENUMERATION, 1, FLOW_NAMES},
   {"columnGap", Kind::SIGNED_INTEGER, 1, {}},
   {"rowGap", Kind::SIGNED_INTEGER, 1, {}},
   {"padding.left", Kind::SIGNED_INTEGER, 2, {}},
   {"padding.top", Kind::SIGNED_INTEGER, 2, {}},
   {"padding.right", Kind::SIGNED_INTEGER, 2, {}},
   {"padding.bottom", Kind::SIGNED_INTEGER, 2, {}},
   {"maxTracksPerAxis", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxPlacements", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxPlacementWork", Kind::UNSIGNED_INTEGER, 1, {}},
   {"scroll", Kind::BOOLEAN, 1, {}},
   {"mainAlign", Kind::ENUMERATION, 1, ALIGN_NAMES},
   {"crossAlign", Kind::ENUMERATION, 1, ALIGN_NAMES},
   {"overflow", Kind::ENUMERATION, 1, OVERFLOW_NAMES},
   {"limits.maxInputBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"limits.maxClusterBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"limits.maxGraphemes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"limits.maxCells", Kind::UNSIGNED_INTEGER, 2, {}},
   {"chrome", Kind::ENUMERATION, 1, CHROME_NAMES},
   {"glyphs", Kind::ENUMERATION, 1, GLYPH_NAMES},
   {"maxPaintCells", Kind::UNSIGNED_INTEGER, 1, {}},
   {"value", Kind::FLOATING_POINT, 1, {}},
   {"minimum", Kind::FLOATING_POINT, 1, {}},
   {"maximum", Kind::FLOATING_POINT, 1, {}},
   {"preferredWidth", Kind::SIGNED_INTEGER, 1, {}},
   {"wheelStep", Kind::FLOATING_POINT, 1, {}},
   {"keyStep", Kind::UNSIGNED_INTEGER, 1, {}},
   {"pageOverlap", Kind::UNSIGNED_INTEGER, 1, {}},
   {"minimumThumb", Kind::SIGNED_INTEGER, 1, {}},
   {"focusable", Kind::BOOLEAN, 1, {}},
   {"horizontal", Kind::ENUMERATION, 1, BAR_POLICY_NAMES},
   {"vertical", Kind::ENUMERATION, 1, BAR_POLICY_NAMES},
   {"placement", Kind::ENUMERATION, 1, PLACEMENT_NAMES},
   {"axis", Kind::ENUMERATION, 1, AXIS_NAMES},
   {"frame.enabled", Kind::BOOLEAN, 2, {}},
   {"frame.glyphs", Kind::ENUMERATION, 2, GLYPH_NAMES},
   {"frame.maxPaintCells", Kind::UNSIGNED_INTEGER, 2, {}},
   {"horizontalAlign", Kind::ENUMERATION, 1, ALIGN_NAMES},
   {"verticalAlign", Kind::ENUMERATION, 1, ALIGN_NAMES},
   {"initiallyOpen", Kind::BOOLEAN, 1, {}},
   {"format.maxBytesPerBinding", Kind::UNSIGNED_INTEGER, 2, {}},
   {"format.strokeSeparator", Kind::LITERAL_STRING, 2, {}},
   {"format.alternativeSeparator", Kind::LITERAL_STRING, 2, {}},
   {"format.repeatMarker", Kind::LITERAL_STRING, 2, {}},
   {"format.releaseMarker", Kind::LITERAL_STRING, 2, {}},
   {"format.unknownMarker", Kind::LITERAL_STRING, 2, {}},
   {"emptyText", Kind::LITERAL_STRING, 1, {}},
   {"unavailableText", Kind::LITERAL_STRING, 1, {}},
   {"entrySeparator", Kind::LITERAL_STRING, 1, {}},
   {"overflowMarker", Kind::LITERAL_STRING, 1, {}},
   {"rowHeight", Kind::SIGNED_INTEGER, 1, {}},
   {"overscanRows", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxColumns", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxPreparedCells", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxPreparedTextBytes", Kind::UNSIGNED_INTEGER, 1, {}},
   {"mode", Kind::ENUMERATION, 1, CHART_MODE_NAMES},
   {"chartGlyphSet", Kind::ENUMERATION, 1, CHART_GLYPH_NAMES},
   {"candleGap", Kind::SIGNED_INTEGER, 1, {}},
   {"yRange.enabled", Kind::BOOLEAN, 2, {}},
   {"yRange.minimum", Kind::FLOATING_POINT, 2, {}},
   {"yRange.maximum", Kind::FLOATING_POINT, 2, {}},
   {"showStatus", Kind::BOOLEAN, 1, {}},
   {"statusRows", Kind::SIGNED_INTEGER, 1, {}},
   {"contentPolicy", Kind::ENUMERATION, 1, CONTENT_POLICY_NAMES},
   {"spinnerInterval", Kind::UNSIGNED_INTEGER, 1, {}},
   {"labels.loading", Kind::TEXT, 2, {}},
   {"labels.empty", Kind::TEXT, 2, {}},
   {"labels.error", Kind::TEXT, 2, {}},
   {"labels.offline", Kind::TEXT, 2, {}},
   {"labels.refreshing", Kind::TEXT, 2, {}},
   {"labels.retry", Kind::TEXT, 2, {}},
   {"maxLayers", Kind::UNSIGNED_INTEGER, 1, {}},
   {"title", Kind::TEXT, 1, {}},
   {"dialog.mode", Kind::ENUMERATION, 2, DIALOG_MODE_NAMES},
   {"dialog.placement", Kind::ENUMERATION, 2, DIALOG_PLACEMENT_NAMES},
   {"dialog.maximumPanelSize.width", Kind::SIGNED_INTEGER, 3, {}},
   {"dialog.maximumPanelSize.height", Kind::SIGNED_INTEGER, 3, {}},
   {"dialog.viewportMargin", Kind::SIGNED_INTEGER, 2, {}},
   {"dialog.anchorGap", Kind::SIGNED_INTEGER, 2, {}},
   {"dialog.closeOnEscape", Kind::BOOLEAN, 2, {}},
   {"dialog.frame.glyphs", Kind::ENUMERATION, 3, GLYPH_NAMES},
   {"dialog.frame.maxPaintCells", Kind::UNSIGNED_INTEGER, 3, {}},
   {"body", Kind::TEXT, 1, {}},
   {"message.labels.acknowledge", Kind::TEXT, 3, {}},
   {"message.labels.confirm", Kind::TEXT, 3, {}},
   {"message.labels.save", Kind::TEXT, 3, {}},
   {"message.labels.discard", Kind::TEXT, 3, {}},
   {"message.labels.cancel", Kind::TEXT, 3, {}},
   {"message.defaultChoice.enabled", Kind::BOOLEAN, 3, {}},
   {"message.defaultChoice.value", Kind::ENUMERATION, 3, MESSAGE_CHOICE_NAMES},
   {"message.allowDestructiveDefault", Kind::BOOLEAN, 2, {}},
   {"message.textLimits.maxInputBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"message.textLimits.maxClusterBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"message.textLimits.maxGraphemes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"message.textLimits.maxCells", Kind::UNSIGNED_INTEGER, 3, {}},
   {"message.maxBodyLines", Kind::UNSIGNED_INTEGER, 2, {}},
   {"notification.capacity", Kind::UNSIGNED_INTEGER, 2, {}},
   {"notification.maxVisible", Kind::UNSIGNED_INTEGER, 2, {}},
   {"notification.placement", Kind::ENUMERATION, 2, TOAST_PLACEMENT_NAMES},
   {"notification.maximumWidth", Kind::SIGNED_INTEGER, 2, {}},
   {"notification.margin", Kind::SIGNED_INTEGER, 2, {}},
   {"notification.gap", Kind::SIGNED_INTEGER, 2, {}},
   {"notification.overflow", Kind::ENUMERATION, 2, TOAST_OVERFLOW_NAMES},
   {"notification.refreshDeduplicated", Kind::BOOLEAN, 2, {}},
   {"notification.reducedMotion", Kind::BOOLEAN, 2, {}},
   {"notification.textLimits.maxInputBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"notification.textLimits.maxClusterBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"notification.textLimits.maxGraphemes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"notification.textLimits.maxCells", Kind::UNSIGNED_INTEGER, 3, {}},
   {"notification.actionLabelBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"notification.dedupKeyBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"notification.maxPaintCells", Kind::UNSIGNED_INTEGER, 2, {}},
   {"themeSelector.labels.apply", Kind::TEXT, 3, {}},
   {"themeSelector.labels.cancel", Kind::TEXT, 3, {}},
   {"themeSelector.labels.closeKeepingPreview", Kind::TEXT, 3, {}},
   {"themeSelector.labels.emptyStatus", Kind::TEXT, 3, {}},
   {"themeSelector.labels.errorStatus", Kind::TEXT, 3, {}},
   {"themeSelector.labels.conflictStatus", Kind::TEXT, 3, {}},
   {"themeSelector.textLimits.maxInputBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeSelector.textLimits.maxClusterBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeSelector.textLimits.maxGraphemes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeSelector.textLimits.maxCells", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeSelector.openingShortcut.enabled", Kind::BOOLEAN, 3, {}},
   {"themeSelector.openingShortcut.value", Kind::LITERAL_STRING, 3, {}},
   {"themeCatalog.maxChoices", Kind::UNSIGNED_INTEGER, 2, {}},
   {"themeCatalog.maxIdBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"themeCatalog.maxLabelBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"themeCatalog.maxDescriptionBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"themeCatalog.maxTextBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"themeCatalog.textLimits.maxInputBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.textLimits.maxClusterBytes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.textLimits.maxGraphemes", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.textLimits.maxCells", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.initialId.enabled", Kind::BOOLEAN, 3, {}},
   {"themeCatalog.initialId.value", Kind::LITERAL_STRING, 3, {}},
   {"themeCatalog.layers.enabled", Kind::BOOLEAN, 3, {}},
   {"themeCatalog.layers.maxLayers", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.layers.maxRules", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.layers.maxDeclarations", Kind::UNSIGNED_INTEGER, 3, {}},
   {"themeCatalog.layers.maxOwnedBytes", Kind::UNSIGNED_INTEGER, 3, {}}}};
static_assert(std::ranges::all_of(PROPERTIES, [](const auto &property) {
  return !property.path.empty();
}));

template <class... Indices> constexpr Mask propertyMask(Indices... indices) {
  Mask mask;

  (mask.set(indices), ...);

  return mask;
}

constexpr Mask SIZE_MASK = propertyMask(1, 2, 3);
constexpr Mask TEXT_MASK = propertyMask(0);
constexpr Mask BUTTON_MASK = SIZE_MASK | TEXT_MASK | propertyMask(37);
constexpr Mask BORDER_MASK = SIZE_MASK | propertyMask(38, 39);
constexpr Mask GAUGE_MASK = BORDER_MASK | propertyMask(40, 41, 42, 43);
constexpr Mask SCROLL_COMMON_MASK = SIZE_MASK | propertyMask(8, 38, 39, 44, 45, 46, 47, 48);
constexpr Mask SCROLL_AREA_MASK = SCROLL_COMMON_MASK | propertyMask(49, 50, 51, 53, 54, 55);
constexpr Mask SCROLL_BAR_MASK = SCROLL_COMMON_MASK | propertyMask(52);
constexpr Mask OVERLAY_MASK = SIZE_MASK | propertyMask(22, 23, 24, 25, 56, 57);
constexpr Mask KEY_HELP_COMMON_MASK = SIZE_MASK | propertyMask(59, 60, 61, 62, 63, 64, 65, 66);
constexpr Mask HELP_VIEW_MASK = KEY_HELP_COMMON_MASK | propertyMask(58);
constexpr Mask KEY_HINTS_MASK = KEY_HELP_COMMON_MASK | propertyMask(67, 68);
constexpr Mask VIRTUAL_LIST_MASK = SIZE_MASK | propertyMask(69, 70);
constexpr Mask VIRTUAL_TABLE_MASK = VIRTUAL_LIST_MASK | propertyMask(71, 72, 73);
constexpr Mask CHART_MASK = propertyMask(74, 75, 76, 77, 78, 79);
constexpr Mask DATA_STATE_MASK = SIZE_MASK | propertyMask(80, 81, 82, 83, 84, 85, 86, 87, 88, 89);
constexpr Mask OVERLAY_HOST_MASK = SIZE_MASK | propertyMask(90);
constexpr Mask DIALOG_MASK = propertyMask(91, 92, 93, 94, 95, 96, 97, 98, 99, 100);
constexpr Mask MESSAGE_MASK =
  DIALOG_MASK | propertyMask(101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114);
constexpr Mask NOTIFICATION_MASK =
  OVERLAY_MASK | propertyMask(115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130);
constexpr Mask THEME_SELECTOR_MASK =
  DIALOG_MASK | propertyMask(131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142);
constexpr Mask THEME_CATALOG_MASK =
  THEME_SELECTOR_MASK | propertyMask(143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158);
constexpr Mask POSITIVE_UNSIGNED_MASK =
  propertyMask(3, 17, 18, 26, 27, 28, PAINT_LIMIT_PROPERTY, 45, 55, 59, 71, 72, 73, 83, 90, 100, 110, 111, 112, 113,
               114, 115, 116, 124, 125, 126, 127, 128, 129, 130, 137, 138, 139, 140, 143, 144, 145, 146, 147, 148, 149,
               150, 151, 155, 156, 157, 158);
constexpr Mask CHECK_MASK = SIZE_MASK | TEXT_MASK | propertyMask(5, 6);
constexpr Mask LAYOUT_MASK = SIZE_MASK | propertyMask(4, 22, 23, 24, 25, 30, 31, 32);
constexpr Mask EDIT_MASK = SIZE_MASK | TEXT_MASK | propertyMask(7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 33, 34, 35, 36);
constexpr Mask ALL_MASK = [] {
  Mask mask;

  for (std::size_t index = 0; index < PROPERTIES.size(); ++index) {
    mask.set(index);
  }

  return mask;
}();
constexpr Mask GRID_MASK = SIZE_MASK | propertyMask(17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29);
constexpr std::size_t BUILTIN_COUNT = 7;
constexpr auto SIZE_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[4], PROPERTIES[22], PROPERTIES[23],
                 PROPERTIES[24], PROPERTIES[25], PROPERTIES[30], PROPERTIES[31], PROPERTIES[32]});
constexpr auto TEXT_SCHEMA = std::to_array({PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3]});
constexpr auto BUTTON_SCHEMA =
  std::to_array({PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[37]});
constexpr auto BORDER_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[38], PROPERTIES[39]});
constexpr auto GAUGE_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[38], PROPERTIES[39], PROPERTIES[40],
                 PROPERTIES[41], PROPERTIES[42], PROPERTIES[43]});
constexpr auto CHECK_SCHEMA =
  std::to_array({PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[5], PROPERTIES[6]});
constexpr auto SCROLL_AREA_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[8], PROPERTIES[38], PROPERTIES[39],
                 PROPERTIES[44], PROPERTIES[45], PROPERTIES[46], PROPERTIES[47], PROPERTIES[48], PROPERTIES[49],
                 PROPERTIES[50], PROPERTIES[51], PROPERTIES[53], PROPERTIES[54], PROPERTIES[55]});
constexpr auto SCROLL_BAR_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[8], PROPERTIES[38], PROPERTIES[39],
                 PROPERTIES[44], PROPERTIES[45], PROPERTIES[46], PROPERTIES[47], PROPERTIES[48], PROPERTIES[52]});
constexpr auto OVERLAY_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[22], PROPERTIES[23], PROPERTIES[24],
                 PROPERTIES[25], PROPERTIES[56], PROPERTIES[57]});
constexpr auto HELP_VIEW_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[58], PROPERTIES[59], PROPERTIES[60],
                 PROPERTIES[61], PROPERTIES[62], PROPERTIES[63], PROPERTIES[64], PROPERTIES[65], PROPERTIES[66]});
constexpr auto KEY_HINTS_SCHEMA = std::to_array(
  {PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[59], PROPERTIES[60], PROPERTIES[61], PROPERTIES[62],
   PROPERTIES[63], PROPERTIES[64], PROPERTIES[65], PROPERTIES[66], PROPERTIES[67], PROPERTIES[68]});
constexpr auto VIRTUAL_LIST_SCHEMA =
  std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[69], PROPERTIES[70]});
constexpr auto VIRTUAL_TABLE_SCHEMA = std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[69],
                                                     PROPERTIES[70], PROPERTIES[71], PROPERTIES[72], PROPERTIES[73]});
constexpr auto CHART_SCHEMA =
  std::to_array({PROPERTIES[74], PROPERTIES[75], PROPERTIES[76], PROPERTIES[77], PROPERTIES[78], PROPERTIES[79]});
constexpr auto DATA_STATE_SCHEMA = std::to_array(
  {PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[80], PROPERTIES[81], PROPERTIES[82], PROPERTIES[83],
   PROPERTIES[84], PROPERTIES[85], PROPERTIES[86], PROPERTIES[87], PROPERTIES[88], PROPERTIES[89]});
constexpr auto OVERLAY_HOST_SCHEMA = std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[90]});
constexpr auto DIALOG_SCHEMA =
  std::to_array({PROPERTIES[91], PROPERTIES[92], PROPERTIES[93], PROPERTIES[94], PROPERTIES[95], PROPERTIES[96],
                 PROPERTIES[97], PROPERTIES[98], PROPERTIES[99], PROPERTIES[100]});
constexpr auto MESSAGE_SCHEMA =
  std::to_array({PROPERTIES[91],  PROPERTIES[92],  PROPERTIES[93],  PROPERTIES[94],  PROPERTIES[95],  PROPERTIES[96],
                 PROPERTIES[97],  PROPERTIES[98],  PROPERTIES[99],  PROPERTIES[100], PROPERTIES[101], PROPERTIES[102],
                 PROPERTIES[103], PROPERTIES[104], PROPERTIES[105], PROPERTIES[106], PROPERTIES[107], PROPERTIES[108],
                 PROPERTIES[109], PROPERTIES[110], PROPERTIES[111], PROPERTIES[112], PROPERTIES[113], PROPERTIES[114]});
constexpr auto NOTIFICATION_SCHEMA =
  std::to_array({PROPERTIES[1],   PROPERTIES[2],   PROPERTIES[3],   PROPERTIES[22],  PROPERTIES[23],
                 PROPERTIES[24],  PROPERTIES[25],  PROPERTIES[56],  PROPERTIES[57],  PROPERTIES[115],
                 PROPERTIES[116], PROPERTIES[117], PROPERTIES[118], PROPERTIES[119], PROPERTIES[120],
                 PROPERTIES[121], PROPERTIES[122], PROPERTIES[123], PROPERTIES[124], PROPERTIES[125],
                 PROPERTIES[126], PROPERTIES[127], PROPERTIES[128], PROPERTIES[129], PROPERTIES[130]});
constexpr auto THEME_SELECTOR_SCHEMA =
  std::to_array({PROPERTIES[91],  PROPERTIES[92],  PROPERTIES[93],  PROPERTIES[94],  PROPERTIES[95],  PROPERTIES[96],
                 PROPERTIES[97],  PROPERTIES[98],  PROPERTIES[99],  PROPERTIES[100], PROPERTIES[131], PROPERTIES[132],
                 PROPERTIES[133], PROPERTIES[134], PROPERTIES[135], PROPERTIES[136], PROPERTIES[137], PROPERTIES[138],
                 PROPERTIES[139], PROPERTIES[140], PROPERTIES[141], PROPERTIES[142]});
constexpr auto THEME_CATALOG_SCHEMA =
  std::to_array({PROPERTIES[91],  PROPERTIES[92],  PROPERTIES[93],  PROPERTIES[94],  PROPERTIES[95],  PROPERTIES[96],
                 PROPERTIES[97],  PROPERTIES[98],  PROPERTIES[99],  PROPERTIES[100], PROPERTIES[131], PROPERTIES[132],
                 PROPERTIES[133], PROPERTIES[134], PROPERTIES[135], PROPERTIES[136], PROPERTIES[137], PROPERTIES[138],
                 PROPERTIES[139], PROPERTIES[140], PROPERTIES[141], PROPERTIES[142], PROPERTIES[143], PROPERTIES[144],
                 PROPERTIES[145], PROPERTIES[146], PROPERTIES[147], PROPERTIES[148], PROPERTIES[149], PROPERTIES[150],
                 PROPERTIES[151], PROPERTIES[152], PROPERTIES[153], PROPERTIES[154], PROPERTIES[155], PROPERTIES[156],
                 PROPERTIES[157], PROPERTIES[158]});
constexpr auto EDIT_SCHEMA =
  std::to_array({PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[7], PROPERTIES[8],
                 PROPERTIES[9], PROPERTIES[10], PROPERTIES[11], PROPERTIES[12], PROPERTIES[13], PROPERTIES[14],
                 PROPERTIES[15], PROPERTIES[16], PROPERTIES[33], PROPERTIES[34], PROPERTIES[35], PROPERTIES[36]});
constexpr auto GRID_SCHEMA = std::to_array(
  {PROPERTIES[17], PROPERTIES[18], PROPERTIES[19], PROPERTIES[20], PROPERTIES[21], PROPERTIES[22], PROPERTIES[23],
   PROPERTIES[24], PROPERTIES[25], PROPERTIES[26], PROPERTIES[27], PROPERTIES[28], PROPERTIES[29]});

bool builtin(std::string_view name) noexcept {
  return name == "VBox" || name == "HBox" || name == "Label" || name == "Button" || name == "CheckBox" ||
         name == "LineEdit" || name == "ThreeState";
}

Mask builtinMask(std::string_view name) noexcept {
  if (name == "Button") {
    return BUTTON_MASK;
  }

  if (name == "Border") {
    return BORDER_MASK;
  }

  if (name == "Gauge") {
    return GAUGE_MASK;
  }
  if (name == "VBox" || name == "HBox") {
    return LAYOUT_MASK;
  }

  if (name == "CheckBox" || name == "ThreeState") {
    return CHECK_MASK;
  }

  if (name == "LineEdit") {
    return EDIT_MASK;
  }

  return TEXT_MASK | SIZE_MASK;
}

bool identifier(std::string_view name) noexcept {
  const auto first = [](unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
  };

  if (name.empty() || !first(static_cast<unsigned char>(name.front()))) {
    return false;
  }

  return std::ranges::all_of(name.substr(1), [&](unsigned char ch) {
    return first(ch) || (ch >= '0' && ch <= '9');
  });
}

std::size_t propertyIndex(std::string_view path) noexcept {
  for (std::size_t index = 0; index < PROPERTIES.size(); ++index) {
    if (PROPERTIES[index].path == path) {
      return index;
    }
  }

  return PROPERTIES.size();
}

std::shared_ptr<const ControlSourceOrigin> sourceFor(const ControlSettings &settings, std::string_view path) noexcept {
  for (const auto &entry : settings.origins) {
    if (entry.path == path) {
      return entry.source;
    }
  }

  return settings.origin;
}

ControlContextError error(Code code, std::string_view type, std::string_view path = {},
                          std::shared_ptr<const ControlSourceOrigin> origin = {}) {
  auto detail = std::make_shared<ControlContextDiagnostic>();
  detail->path = type;

  if (!path.empty()) {
    detail->path += '.';
    detail->path += path;
  }

  detail->origin = std::move(origin);

  return {code, std::move(detail)};
}

ControlContextError exceptionError() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc &) {
    return {Code::NO_MEMORY, {}};
  } catch (const std::length_error &) {
    return {Code::NO_MEMORY, {}};
  } catch (...) {
    return {Code::CONFIGURATION_FAILED, {}};
  }
}

// This finite list is the complete scalar vocabulary; there is no field reflection.
#define PORTALS_CONTROL_LEAVES(X)                                                                                      \
  X(text, 0)                                                                                                           \
  X(width, 1)                                                                                                          \
  X(height, 2)                                                                                                         \
  X(flex, 3)                                                                                                           \
  X(gap, 4)                                                                                                            \
  X(state, 5)                                                                                                          \
  X(cycle, 6)                                                                                                          \
  X(editMode, 7)                                                                                                       \
  X(readOnly, 8)                                                                                                       \
  X(programmaticChange, 9)                                                                                             \
  X(displayMode, 10)                                                                                                   \
  X(placeholder, 11)                                                                                                   \
  X(history.enabled, 12)                                                                                               \
  X(history.maxOperations, 13)                                                                                         \
  X(history.maxBytes, 14)                                                                                              \
  X(history.groupTyping, 15)                                                                                           \
  X(history.groupDeletion, 16)                                                                                         \
  X(grid.columns, 17)                                                                                                  \
  X(grid.rows, 18)                                                                                                     \
  X(grid.flow, 19)                                                                                                     \
  X(grid.columnGap, 20)                                                                                                \
  X(grid.rowGap, 21)                                                                                                   \
  X(grid.paddingLeft, 22)                                                                                              \
  X(grid.paddingTop, 23)                                                                                               \
  X(grid.paddingRight, 24)                                                                                             \
  X(grid.paddingBottom, 25)                                                                                            \
  X(grid.maxTracksPerAxis, 26)                                                                                         \
  X(grid.maxPlacements, 27)                                                                                            \
  X(grid.maxPlacementWork, 28)                                                                                         \
  X(grid.scroll, 29)                                                                                                   \
  X(linear.mainAlign, 30)                                                                                              \
  X(linear.crossAlign, 31)                                                                                             \
  X(linear.overflow, 32)                                                                                               \
  X(limits.maxInputBytes, 33)                                                                                          \
  X(limits.maxClusterBytes, 34)                                                                                        \
  X(limits.maxGraphemes, 35)                                                                                           \
  X(limits.maxCells, 36)                                                                                               \
  X(chrome, 37)                                                                                                        \
  X(glyphs, 38)                                                                                                        \
  X(maxPaintCells, 39)                                                                                                 \
  X(value, 40)                                                                                                         \
  X(minimum, 41)                                                                                                       \
  X(maximum, 42)                                                                                                       \
  X(preferredWidth, 43)                                                                                                \
  X(wheelStep, 44)                                                                                                     \
  X(keyStep, 45)                                                                                                       \
  X(pageOverlap, 46)                                                                                                   \
  X(minimumThumb, 47)                                                                                                  \
  X(focusable, 48)                                                                                                     \
  X(horizontal, 49)                                                                                                    \
  X(vertical, 50)                                                                                                      \
  X(placement, 51)                                                                                                     \
  X(axis, 52)                                                                                                          \
  X(frame.enabled, 53)                                                                                                 \
  X(frame.glyphs, 54)                                                                                                  \
  X(frame.maxPaintCells, 55)                                                                                           \
  X(horizontalAlign, 56)                                                                                               \
  X(verticalAlign, 57)                                                                                                 \
  X(initiallyOpen, 58)                                                                                                 \
  X(format.maxBytesPerBinding, 59)                                                                                     \
  X(format.strokeSeparator, 60)                                                                                        \
  X(format.alternativeSeparator, 61)                                                                                   \
  X(format.repeatMarker, 62)                                                                                           \
  X(format.releaseMarker, 63)                                                                                          \
  X(format.unknownMarker, 64)                                                                                          \
  X(emptyText, 65)                                                                                                     \
  X(unavailableText, 66)                                                                                               \
  X(entrySeparator, 67)                                                                                                \
  X(overflowMarker, 68)                                                                                                \
  X(rowHeight, 69)                                                                                                     \
  X(overscanRows, 70)                                                                                                  \
  X(maxColumns, 71)                                                                                                    \
  X(maxPreparedCells, 72)                                                                                              \
  X(maxPreparedTextBytes, 73)                                                                                          \
  X(mode, 74)                                                                                                          \
  X(chartGlyphSet, 75)                                                                                                 \
  X(candleGap, 76)                                                                                                     \
  X(yRange.enabled, 77)                                                                                                \
  X(yRange.minimum, 78)                                                                                                \
  X(yRange.maximum, 79)                                                                                                \
  X(showStatus, 80)                                                                                                    \
  X(statusRows, 81)                                                                                                    \
  X(contentPolicy, 82)                                                                                                 \
  X(spinnerInterval, 83)                                                                                               \
  X(labels.loading, 84)                                                                                                \
  X(labels.empty, 85)                                                                                                  \
  X(labels.error, 86)                                                                                                  \
  X(labels.offline, 87)                                                                                                \
  X(labels.refreshing, 88)                                                                                             \
  X(labels.retry, 89)                                                                                                  \
  X(maxLayers, 90)                                                                                                     \
  X(title, 91)                                                                                                         \
  X(dialog.mode, 92)                                                                                                   \
  X(dialog.placement, 93)                                                                                              \
  X(dialog.maximumPanelSize.width, 94)                                                                                 \
  X(dialog.maximumPanelSize.height, 95)                                                                                \
  X(dialog.viewportMargin, 96)                                                                                         \
  X(dialog.anchorGap, 97)                                                                                              \
  X(dialog.closeOnEscape, 98)                                                                                          \
  X(dialog.frame.glyphs, 99)                                                                                           \
  X(dialog.frame.maxPaintCells, 100)                                                                                   \
  X(body, 101)                                                                                                         \
  X(message.labels.acknowledge, 102)                                                                                   \
  X(message.labels.confirm, 103)                                                                                       \
  X(message.labels.save, 104)                                                                                          \
  X(message.labels.discard, 105)                                                                                       \
  X(message.labels.cancel, 106)                                                                                        \
  X(message.defaultChoice.enabled, 107)                                                                                \
  X(message.defaultChoice.value, 108)                                                                                  \
  X(message.allowDestructiveDefault, 109)                                                                              \
  X(message.textLimits.maxInputBytes, 110)                                                                             \
  X(message.textLimits.maxClusterBytes, 111)                                                                           \
  X(message.textLimits.maxGraphemes, 112)                                                                              \
  X(message.textLimits.maxCells, 113)                                                                                  \
  X(message.maxBodyLines, 114)                                                                                         \
  X(notification.capacity, 115)                                                                                        \
  X(notification.maxVisible, 116)                                                                                      \
  X(notification.placement, 117)                                                                                       \
  X(notification.maximumWidth, 118)                                                                                    \
  X(notification.margin, 119)                                                                                          \
  X(notification.gap, 120)                                                                                             \
  X(notification.overflow, 121)                                                                                        \
  X(notification.refreshDeduplicated, 122)                                                                             \
  X(notification.reducedMotion, 123)                                                                                   \
  X(notification.textLimits.maxInputBytes, 124)                                                                        \
  X(notification.textLimits.maxClusterBytes, 125)                                                                      \
  X(notification.textLimits.maxGraphemes, 126)                                                                         \
  X(notification.textLimits.maxCells, 127)                                                                             \
  X(notification.actionLabelBytes, 128)                                                                                \
  X(notification.dedupKeyBytes, 129)                                                                                   \
  X(notification.maxPaintCells, 130)                                                                                   \
  X(themeSelector.labels.apply, 131)                                                                                   \
  X(themeSelector.labels.cancel, 132)                                                                                  \
  X(themeSelector.labels.closeKeepingPreview, 133)                                                                     \
  X(themeSelector.labels.emptyStatus, 134)                                                                             \
  X(themeSelector.labels.errorStatus, 135)                                                                             \
  X(themeSelector.labels.conflictStatus, 136)                                                                          \
  X(themeSelector.textLimits.maxInputBytes, 137)                                                                       \
  X(themeSelector.textLimits.maxClusterBytes, 138)                                                                     \
  X(themeSelector.textLimits.maxGraphemes, 139)                                                                        \
  X(themeSelector.textLimits.maxCells, 140)                                                                            \
  X(themeSelector.openingShortcut.enabled, 141)                                                                        \
  X(themeSelector.openingShortcut.value, 142)                                                                          \
  X(themeCatalog.maxChoices, 143)                                                                                      \
  X(themeCatalog.maxIdBytes, 144)                                                                                      \
  X(themeCatalog.maxLabelBytes, 145)                                                                                   \
  X(themeCatalog.maxDescriptionBytes, 146)                                                                             \
  X(themeCatalog.maxTextBytes, 147)                                                                                    \
  X(themeCatalog.textLimits.maxInputBytes, 148)                                                                        \
  X(themeCatalog.textLimits.maxClusterBytes, 149)                                                                      \
  X(themeCatalog.textLimits.maxGraphemes, 150)                                                                         \
  X(themeCatalog.textLimits.maxCells, 151)                                                                             \
  X(themeCatalog.initialId.enabled, 152)                                                                               \
  X(themeCatalog.initialId.value, 153)                                                                                 \
  X(themeCatalog.layers.enabled, 154)                                                                                  \
  X(themeCatalog.layers.maxLayers, 155)                                                                                \
  X(themeCatalog.layers.maxRules, 156)                                                                                 \
  X(themeCatalog.layers.maxDeclarations, 157)                                                                          \
  X(themeCatalog.layers.maxOwnedBytes, 158)

#define LEAF_INDEX(field, index) index,
constexpr auto LEAF_INDICES = std::to_array<std::size_t>({PORTALS_CONTROL_LEAVES(LEAF_INDEX)});
#undef LEAF_INDEX
static_assert(LEAF_INDICES.size() == detail::CONTROL_PROPERTY_COUNT);
static_assert([] {
  for (std::size_t index = 0; index < LEAF_INDICES.size(); ++index) {
    if (LEAF_INDICES[index] != index) {
      return false;
    }
  }

  return true;
}());

template <class Visitor> void visitLeaves(const ControlSettings &settings, Visitor &&visitor) {
#define VISIT(field, index) visitor(index, settings.field);
  PORTALS_CONTROL_LEAVES(VISIT)
#undef VISIT
}

std::size_t mergedLeafCount(const ControlSettings &left, const ControlSettings &right) {
  return (detail::controlSettingsPresenceSet(left) | detail::controlSettingsPresenceSet(right)).count();
}

void merge(ControlSettings &target, const ControlSettings &patch) {
  const auto assign = [&](auto &destination, const auto &value, std::size_t index) {
    if (!value) {
      return;
    }

    destination = value;
    const auto path = PROPERTIES[index].path;
    auto found = std::ranges::find(target.origins, path, &ControlPropertyOrigin::path);

    if (found == target.origins.end()) {
      ControlPropertyOrigin provenance;
      provenance.path = path;
      provenance.source = sourceFor(patch, path);
      target.origins.push_back(provenance);
    } else {
      found->source = sourceFor(patch, path);
    }
  };
#define ASSIGN(field, index) assign(target.field, patch.field, index);
  PORTALS_CONTROL_LEAVES(ASSIGN)
#undef ASSIGN
}

#undef PORTALS_CONTROL_LEAVES

template <class T> bool validValue(const T &value, std::size_t index) noexcept {
  if constexpr (std::same_as<T, std::int32_t>) {
    const bool positive = index == MINIMUM_THUMB_PROPERTY || index == ROW_HEIGHT_PROPERTY ||
                          index == STATUS_ROWS_PROPERTY || index == DIALOG_WIDTH_PROPERTY ||
                          index == DIALOG_HEIGHT_PROPERTY || index == NOTIFICATION_WIDTH_PROPERTY;

    return positive ? value > 0 : value >= 0;
  } else if constexpr (std::unsigned_integral<T> && !std::same_as<T, bool>) {
    return index == 155 ? value >= 2 : !POSITIVE_UNSIGNED_MASK.test(index) || value > 0;
  } else if constexpr (std::same_as<T, std::string>) {
    return detail::validControlLiteral(value) && (index != 68 || !value.empty());
  } else if constexpr (std::same_as<T, double>) {
    return std::isfinite(value) && (index != WHEEL_STEP_PROPERTY || value > 0);
  } else if constexpr (std::same_as<T, ToastPlacement>) {
    return value == ToastPlacement::BOTTOM_END || value == ToastPlacement::TOP_END ||
           value == ToastPlacement::BOTTOM_START || value == ToastPlacement::TOP_START;
  } else if constexpr (std::same_as<T, ToastOverflowPolicy>) {
    return value == ToastOverflowPolicy::DROP_OLDEST_PENDING || value == ToastOverflowPolicy::REJECT_NEW;
  } else if constexpr (std::same_as<T, MessageChoice>) {
    return value == MessageChoice::ACKNOWLEDGE || value == MessageChoice::CONFIRM || value == MessageChoice::SAVE ||
           value == MessageChoice::DISCARD || value == MessageChoice::CANCEL;
  } else if constexpr (std::same_as<T, DialogMode>) {
    return value == DialogMode::MODAL || value == DialogMode::NON_MODAL;
  } else if constexpr (std::same_as<T, DialogPlacement>) {
    return value == DialogPlacement::CENTER || value == DialogPlacement::BELOW_START ||
           value == DialogPlacement::ABOVE_START;
  } else if constexpr (std::same_as<T, DataContentPolicy>) {
    return value == DataContentPolicy::KEEP_BODY || value == DataContentPolicy::REPLACE;
  } else if constexpr (std::same_as<T, ChartMode>) {
    return value == ChartMode::LINE || value == ChartMode::CANDLES;
  } else if constexpr (std::same_as<T, ChartGlyphSet>) {
    return value == ChartGlyphSet::ASCII || value == ChartGlyphSet::BOX_DRAWING;
  } else if constexpr (std::same_as<T, ScrollAxis>) {
    return value == ScrollAxis::HORIZONTAL || value == ScrollAxis::VERTICAL;
  } else if constexpr (std::same_as<T, ScrollBarPolicy>) {
    return value == ScrollBarPolicy::NEVER || value == ScrollBarPolicy::AUTO || value == ScrollBarPolicy::ALWAYS;
  } else if constexpr (std::same_as<T, ScrollPlacement>) {
    return value == ScrollPlacement::RESERVED || value == ScrollPlacement::OVERLAY ||
           value == ScrollPlacement::ON_FRAME;
  } else if constexpr (std::same_as<T, ButtonChrome>) {
    return value == ButtonChrome::BRACKETS || value == ButtonChrome::NONE;
  } else if constexpr (std::same_as<T, PrimitiveGlyphs>) {
    return value == PrimitiveGlyphs{} || value == PrimitiveGlyphs::ASCII;
  } else if constexpr (std::same_as<T, GridFlow>) {
    return value == GridFlow::ROW_MAJOR || value == GridFlow::COLUMN_MAJOR;
  } else if constexpr (std::same_as<T, LayoutAlign>) {
    return value == LayoutAlign::START || value == LayoutAlign::CENTER || value == LayoutAlign::END ||
           value == LayoutAlign::STRETCH;
  } else if constexpr (std::same_as<T, LayoutOverflow>) {
    return value == LayoutOverflow::CLIP;
  } else if constexpr (std::same_as<T, CheckState>) {
    return value == CheckState::UNCHECKED || value == CheckState::CHECKED || value == CheckState::MIXED;
  } else if constexpr (std::same_as<T, CheckCycle>) {
    return value == CheckCycle::TWO_STATE || value == CheckCycle::THREE_STATE;
  } else if constexpr (std::same_as<T, EditMode>) {
    return value == EditMode::IMMEDIATE || value == EditMode::ON_COMPLETION || value == EditMode::EXPLICIT;
  } else if constexpr (std::same_as<T, ProgrammaticChange>) {
    return value == ProgrammaticChange::RECORD || value == ProgrammaticChange::RESET_BASELINE;
  } else if constexpr (std::same_as<T, LineEditDisplayMode>) {
    return value == LineEditDisplayMode::NORMAL || value == LineEditDisplayMode::PASSWORD ||
           value == LineEditDisplayMode::NO_ECHO;
  } else {
    return true;
  }
}

std::expected<void, ControlContextError> validateSettings(const ControlSettings &settings, std::string_view type,
                                                          Mask mask, bool fixedCycle, ControlContextLimits limits) {
  if (settings.origins.size() > PROPERTIES.size()) {
    return std::unexpected(error(Code::DUPLICATE_PROPERTY, type));
  }

  for (std::size_t index = 0; index < settings.origins.size(); ++index) {
    const auto &origin = settings.origins[index];

    if (propertyIndex(origin.path) == PROPERTIES.size()) {
      return std::unexpected(error(Code::UNKNOWN_PROPERTY, type, origin.path, origin.source));
    }

    for (std::size_t previous = 0; previous < index; ++previous) {
      if (settings.origins[previous].path == origin.path) {
        return std::unexpected(error(Code::DUPLICATE_PROPERTY, type, origin.path, origin.source));
      }
    }
  }

  std::optional<ControlContextError> failure;
  std::size_t count = 0;
  visitLeaves(settings, [&](std::size_t index, const auto &value) {
    if (!value || failure) {
      return;
    }

    const auto &descriptor = PROPERTIES[index];
    std::optional<Code> code;

    if (!mask.test(index)) {
      code = Code::UNSUPPORTED_PROPERTY;
    } else if (descriptor.depth > limits.maxStructDepth) {
      code = Code::STRUCT_DEPTH_LIMIT;
    } else if (!validValue(*value, index)) {
      code = Code::INVALID_VALUE;
    }

    if constexpr (std::same_as<std::remove_cvref_t<decltype(*value)>, std::string>) {
      if (!code && index == SELECTOR_SHORTCUT_PROPERTY) {
        const auto parsed = parseShortcut(*value);

        if (!parsed) {
          code = parsed.error().cause == ShortcutErrorKind::NO_MEMORY ? Code::NO_MEMORY : Code::INVALID_VALUE;
        }
      }
    }

    if (!code && ++count > limits.maxMaterializedLeaves) {
      code = Code::MATERIALIZED_LEAF_LIMIT;
    }

    if (code) {
      failure = error(*code, type, descriptor.path, sourceFor(settings, descriptor.path));
    }
  });

  if (failure) {
    return std::unexpected(*failure);
  }

  if (mask == MESSAGE_MASK) {
    if (settings.dialog.mode && *settings.dialog.mode != DialogMode::MODAL) {
      return std::unexpected(error(Code::INVALID_VALUE, type, "dialog.mode", sourceFor(settings, "dialog.mode")));
    }
    if (settings.dialog.closeOnEscape == false) {
      return std::unexpected(
        error(Code::INVALID_VALUE, type, "dialog.closeOnEscape", sourceFor(settings, "dialog.closeOnEscape")));
    }
  }

  if ((mask == THEME_SELECTOR_MASK || mask == THEME_CATALOG_MASK) && settings.dialog.mode &&
      *settings.dialog.mode != DialogMode::MODAL) {
    return std::unexpected(error(Code::INVALID_VALUE, type, "dialog.mode", sourceFor(settings, "dialog.mode")));
  }

  if (fixedCycle && settings.cycle && *settings.cycle != CheckCycle::THREE_STATE) {
    return std::unexpected(error(Code::FIXED_CYCLE, type, "cycle", sourceFor(settings, "cycle")));
  }

  return {};
}
} // namespace

namespace detail {
std::uint32_t controlSettingsPresence(const ControlSettings &settings) noexcept {
  const auto presence = controlSettingsPresenceSet(settings);
  constexpr ControlPropertyPresence LOW_BITS{0xffffffffULL};

  return static_cast<std::uint32_t>((presence & LOW_BITS).to_ullong());
}

ControlPropertyPresence controlSettingsPresenceSet(const ControlSettings &settings) noexcept {
  Mask mask{};
  visitLeaves(settings, [&](std::size_t index, const auto &value) {
    if (value) {
      mask.set(index);
    }
  });

  return mask;
}
} // namespace detail

struct ControlContext::Data {
  struct Entry {
    ~Entry() = default;

    std::string name = std::string(0, '\0');
    std::string base = std::string(0, '\0');
    ControlSettings settings;
  };

  struct Schema {
    ~Schema() = default;

    std::string name = std::string(0, '\0');
    Mask mask{};
    detail::ControlFactoryKind factoryKind{detail::ControlFactoryKind::USER};
  };

  std::optional<Schema> fallback;
  std::array<std::optional<Schema>, 2> primitiveFallbacks;
  std::array<std::optional<Schema>, 2> scrollFallbacks;
  std::optional<Schema> overlayFallback;
  std::optional<Schema> dataStateFallback;
  std::optional<Schema> messageFallback;
  std::optional<Schema> notificationFallback;
  std::optional<Schema> selectorFallback;
  std::optional<Schema> catalogFallback;
  bool catalogRecipient{};
  std::array<std::optional<Schema>, 2> dialogFallbacks;
  std::array<std::optional<Schema>, 2> keyHelpFallbacks;
  std::array<std::optional<Schema>, 3> virtualViewFallbacks;
  std::vector<Entry> definitions = std::vector<Entry>(0);
  std::vector<Entry> defaults = std::vector<Entry>(0);
  std::vector<Schema> schemas = std::vector<Schema>(0);
};

namespace {
struct Chain {
  ~Chain() = default;

  std::vector<std::string_view> names = std::vector<std::string_view>(0);
  std::string_view base;
  Mask mask{};
  bool fixedCycle{};
  std::vector<std::string_view> styleNames = std::vector<std::string_view>(0);
  bool catalogFamily{};
  detail::ControlFactoryKind factoryKind{detail::ControlFactoryKind::USER};
};

template <class Data> auto definition(const Data &data, std::string_view name) {
  return std::ranges::find(data.definitions, name, &Data::Entry::name);
}

template <class Data> const typename Data::Schema *fallbackFor(const Data &data, std::string_view name) {
  if (data.catalogFallback && data.catalogFallback->name == name) {
    return &*data.catalogFallback;
  }
  if (data.selectorFallback && data.selectorFallback->name == name) {
    return &*data.selectorFallback;
  }
  if (data.notificationFallback && data.notificationFallback->name == name) {
    return &*data.notificationFallback;
  }
  if (data.messageFallback && data.messageFallback->name == name) {
    return &*data.messageFallback;
  }
  if (data.dataStateFallback && data.dataStateFallback->name == name) {
    return &*data.dataStateFallback;
  }
  if (data.overlayFallback && data.overlayFallback->name == name) {
    return &*data.overlayFallback;
  }
  if (data.fallback && data.fallback->name == name) {
    return &*data.fallback;
  }

  for (const auto &fallback : data.dialogFallbacks) {
    if (fallback && fallback->name == name) {
      return &*fallback;
    }
  }

  for (const auto &fallback : data.primitiveFallbacks) {
    if (fallback && fallback->name == name) {
      return &*fallback;
    }
  }

  for (const auto &fallback : data.scrollFallbacks) {
    if (fallback && fallback->name == name) {
      return &*fallback;
    }
  }

  for (const auto &fallback : data.keyHelpFallbacks) {
    if (fallback && fallback->name == name) {
      return &*fallback;
    }
  }

  for (const auto &fallback : data.virtualViewFallbacks) {
    if (fallback && fallback->name == name) {
      return &*fallback;
    }
  }

  return nullptr;
}

template <class Data> void catalogAncestry(const Data &data, Chain &chain) {
  const bool catalog = chain.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING;
  const bool view = chain.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING;
  if (!catalog && !(view && data.catalogRecipient)) {
    return;
  }

  // Default ancestry does not add visual base types or consume a user inheritance level.
  chain.styleNames = chain.names;
  chain.catalogFamily = true;
  if (chain.names.back() == "ThemeSelector") {
    chain.names.pop_back();
    chain.names.emplace_back("ThemeSelectorCatalog");
    chain.names.emplace_back("ThemeSelector");
  } else {
    if (chain.names.back() != "ThemeSelectorCatalog") {
      chain.names.emplace_back("ThemeSelectorCatalog");
    }
    chain.names.emplace_back("ThemeSelector");
  }
  if (data.catalogRecipient) {
    chain.base = "ThemeSelectorCatalog";
  }
  chain.factoryKind = detail::ControlFactoryKind::THEME_CATALOG_BINDING;
  chain.mask = THEME_CATALOG_MASK;
}

template <class Data>
std::expected<Chain, ControlContextError> chainFor(const Data &data, std::string_view name, ControlContextLimits limits,
                                                   std::shared_ptr<const ControlSourceOrigin> origin) {
  Chain chain;
  auto current = name;
  auto declaration = name;
  std::string_view property;

  while (true) {
    if (std::ranges::find(chain.names, current) != chain.names.end()) {
      return std::unexpected(error(Code::INHERITANCE_CYCLE, declaration, property, origin));
    }

    if (!chain.names.empty() && chain.names.size() > limits.maxInheritance) {
      return std::unexpected(error(Code::INHERITANCE_LIMIT, declaration, property, origin));
    }

    chain.names.push_back(current);

    if (builtin(current)) {
      chain.base = current;
      chain.factoryKind = detail::ControlFactoryKind::EXISTING_BUILTIN;
      chain.mask = builtinMask(current);
      chain.fixedCycle = current == "ThreeState";

      if (chain.fixedCycle) {
        if (chain.names.size() > limits.maxInheritance) {
          return std::unexpected(error(Code::INHERITANCE_LIMIT, declaration, property, origin));
        }

        chain.names.push_back("CheckBox");
      }

      return chain;
    }

    const auto schema = std::ranges::find(data.schemas, current, &Data::Schema::name);

    if (schema != data.schemas.end()) {
      chain.base = schema->name;
      chain.mask = schema->mask;
      chain.factoryKind = schema->factoryKind;
      catalogAncestry(data, chain);

      return chain;
    }

    const auto found = definition(data, current);

    if (found == data.definitions.end()) {
      if (const auto *fallback = fallbackFor(data, current)) {
        chain.base = fallback->name;
        chain.mask = fallback->mask;
        chain.factoryKind = fallback->factoryKind;
        catalogAncestry(data, chain);
        return chain;
      }
      return std::unexpected(error(Code::UNKNOWN_TYPE, declaration, property, origin));
    }

    declaration = found->name;
    property = "base";
    origin = found->settings.origin;
    current = found->base;
  }
}

template <class Data>
std::expected<ResolvedControl, ControlContextError>
resolveData(const Data &data, std::string_view name, const ControlSettings &instance, ControlContextLimits limits,
            std::shared_ptr<const ControlSourceOrigin> origin = {}) {
  const auto chain = chainFor(data, name, limits, origin ? origin : instance.origin);

  if (!chain) {
    return std::unexpected(chain.error());
  }

  ResolvedControl result;
  result.type = name;
  result.base = chain->base;
  result.factoryKind = chain->factoryKind;

  const auto &styleNames = chain->catalogFamily ? chain->styleNames : chain->names;
  for (std::size_t index = 1; index < styleNames.size(); ++index) {
    result.lineage.emplace_back(styleNames[index]);
  }

  const auto intrinsic = chain->base == "VBox"   ? std::string_view("Column")
                         : chain->base == "HBox" ? std::string_view("Row")
                                                 : std::string_view{};

  if (!intrinsic.empty() && intrinsic != result.type &&
      std::ranges::find(result.lineage, intrinsic) == result.lineage.end()) {
    result.lineage.emplace_back(intrinsic);
  }

  if (result.lineage.size() > limits.maxInheritance) {
    return std::unexpected(error(Code::INHERITANCE_LIMIT, name, {}, origin ? origin : instance.origin));
  }

  for (auto ancestor = chain->names.rbegin(); ancestor != chain->names.rend(); ++ancestor) {
    if (*ancestor == "ThreeState") {
      ControlSettings builtinPatch;
      builtinPatch.cycle = CheckCycle::THREE_STATE;

      if (mergedLeafCount(result.settings, builtinPatch) > limits.maxMaterializedLeaves) {
        return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
      }

      merge(result.settings, builtinPatch);
    }

    const auto found = definition(data, *ancestor);

    if (found != data.definitions.end()) {
      const auto checked = validateSettings(found->settings, found->name, chain->mask,
                                            chain->fixedCycle && *ancestor != "CheckBox", limits);

      if (!checked) {
        return std::unexpected(checked.error());
      }

      if (mergedLeafCount(result.settings, found->settings) > limits.maxMaterializedLeaves) {
        return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
      }

      merge(result.settings, found->settings);
    }

    const auto defaults = std::ranges::find(data.defaults, *ancestor, &Data::Entry::name);

    if (defaults != data.defaults.end()) {
      const auto checked = validateSettings(defaults->settings, defaults->name, chain->mask,
                                            chain->fixedCycle && *ancestor != "CheckBox", limits);

      if (!checked) {
        return std::unexpected(checked.error());
      }

      if (mergedLeafCount(result.settings, defaults->settings) > limits.maxMaterializedLeaves) {
        return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
      }

      merge(result.settings, defaults->settings);
    }
  }

  const auto checked = validateSettings(instance, name, chain->mask, chain->fixedCycle, limits);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  result.materializedLeaves = mergedLeafCount(result.settings, instance);

  if (result.materializedLeaves > limits.maxMaterializedLeaves) {
    return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
  }

  merge(result.settings, instance);

  const auto &notification = result.settings.notification;

  if (notification.capacity && notification.maxVisible && *notification.maxVisible > *notification.capacity) {
    return std::unexpected(error(Code::INVALID_VALUE, name, "notification.maxVisible",
                                 sourceFor(result.settings, "notification.maxVisible")));
  }

  const auto &range = result.settings.yRange;

  // Bound-only patches still depend on receiving options, but a reversed active pair can never be valid.
  if (range.enabled != false && range.minimum && range.maximum && *range.minimum > *range.maximum) {
    return std::unexpected(
      error(Code::INVALID_VALUE, name, "yRange.maximum", sourceFor(result.settings, "yRange.maximum")));
  }

  return result;
}
} // namespace

std::span<const ControlPropertyDescriptor> controlPropertySchema(std::string_view type) noexcept {
  if (type == "VBox" || type == "HBox") {
    return SIZE_SCHEMA;
  }

  if (type == "CheckBox" || type == "ThreeState") {
    return CHECK_SCHEMA;
  }

  if (type == "LineEdit") {
    return EDIT_SCHEMA;
  }

  if (type == "Button") {
    return BUTTON_SCHEMA;
  }

  if (type == "Border") {
    return BORDER_SCHEMA;
  }

  if (type == "Gauge") {
    return GAUGE_SCHEMA;
  }

  if (type == "ScrollArea") {
    return SCROLL_AREA_SCHEMA;
  }

  if (type == "ScrollBar") {
    return SCROLL_BAR_SCHEMA;
  }

  if (type == "Overlay") {
    return OVERLAY_SCHEMA;
  }

  if (type == "HelpView") {
    return HELP_VIEW_SCHEMA;
  }
  if (type == "KeyHints") {
    return KEY_HINTS_SCHEMA;
  }
  if (type == "VirtualList") {
    return VIRTUAL_LIST_SCHEMA;
  }
  if (type == "VirtualTable") {
    return VIRTUAL_TABLE_SCHEMA;
  }
  if (type == "Chart") {
    return CHART_SCHEMA;
  }
  if (type == "DataStateView") {
    return DATA_STATE_SCHEMA;
  }
  if (type == "OverlayHost") {
    return OVERLAY_HOST_SCHEMA;
  }
  if (type == "ThemeSelectorCatalog") {
    return THEME_CATALOG_SCHEMA;
  }
  if (type == "ThemeSelector") {
    return THEME_SELECTOR_SCHEMA;
  }
  if (type == "Notifications") {
    return NOTIFICATION_SCHEMA;
  }
  if (type == "MessageBox") {
    return MESSAGE_SCHEMA;
  }
  if (type == "Dialog") {
    return DIALOG_SCHEMA;
  }

  if (type == "Label") {
    return TEXT_SCHEMA;
  }

  return {};
}

namespace detail {
bool ControlContextAccess::hasDeclaredType(const ControlContext &context, std::string_view name) noexcept {
  return context.data_ && (definition(*context.data_, name) != context.data_->definitions.end() ||
                           std::ranges::find(context.data_->schemas, name, &ControlContext::Data::Schema::name) !=
                             context.data_->schemas.end());
}

bool ControlContextAccess::hasUserConfiguration(const ControlContext &context) noexcept {
  const ControlContextLimits defaults;

  return context.failure_ || context.limits_.maxDefinitions != defaults.maxDefinitions ||
         context.limits_.maxInheritance != defaults.maxInheritance ||
         context.limits_.maxStructDepth != defaults.maxStructDepth ||
         context.limits_.maxMaterializedLeaves != defaults.maxMaterializedLeaves ||
         (context.data_ &&
          (!context.data_->definitions.empty() || !context.data_->defaults.empty() || !context.data_->schemas.empty()));
}

std::span<const ControlPropertyDescriptor> ControlContextAccess::gridSchema() noexcept {
  return GRID_SCHEMA;
}

std::string_view ControlContextAccess::canonicalProperty(std::string_view property) noexcept {
  for (const auto &descriptor : PROPERTIES) {
    if (descriptor.path == property) {
      return descriptor.path;
    }

    for (const auto alias : descriptor.aliases) {
      if (alias == property) {
        return descriptor.path;
      }
    }
  }

  return property;
}

std::expected<void, ControlContextError> ControlContextAccess::installFallbackSchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  if (hasDeclaredType(context, "Grid") || (context.data_ && context.data_->fallback)) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;
    schema.name = "Grid";
    schema.mask = GRID_MASK;
    schema.factoryKind = ControlFactoryKind::DECLARATIVE_GRID_FALLBACK;
    next->fallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installPrimitiveSchema(ControlContext &context,
                                                                                      std::string_view name) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  if (name != "Border" && name != "Gauge") {
    return std::unexpected(ControlContextError{Code::UNKNOWN_TYPE, {}});
  }

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;
    schema.name = name;
    schema.mask = name == "Border" ? BORDER_MASK : GAUGE_MASK;
    schema.factoryKind = ControlFactoryKind::PRIMITIVE_FALLBACK;
    next->primitiveFallbacks[name == "Border" ? 0 : 1] = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installScrollSchema(ControlContext &context,
                                                                                   std::string_view name) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  if (name != "ScrollArea" && name != "ScrollBar") {
    return std::unexpected(ControlContextError{Code::UNKNOWN_TYPE, {}});
  }

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;
    schema.name = name;
    schema.mask = name == "ScrollArea" ? SCROLL_AREA_MASK : SCROLL_BAR_MASK;
    schema.factoryKind = ControlFactoryKind::SCROLL_FALLBACK;
    next->scrollFallbacks[name == "ScrollArea" ? 0 : 1] = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installOverlaySchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }
  constexpr std::string_view NAME = "Overlay";
  if (hasDeclaredType(context, NAME) || (context.data_ && fallbackFor(*context.data_, NAME))) {
    return {};
  }
  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;
    schema.name = NAME;
    schema.mask = OVERLAY_MASK;
    schema.factoryKind = ControlFactoryKind::OVERLAY_FALLBACK;
    next->overlayFallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;
    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError>
ControlContextAccess::installDataStateSchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  constexpr std::string_view NAME = "DataStateView";

  if (hasDeclaredType(context, NAME) || (context.data_ && fallbackFor(*context.data_, NAME))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;

    schema.name = NAME;
    schema.mask = DATA_STATE_MASK;
    schema.factoryKind = ControlFactoryKind::DATA_STATE_FALLBACK;
    next->dataStateFallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError>
ControlContextAccess::installNotificationSchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  constexpr std::string_view name = "Notifications";

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;

    schema.name = name;
    schema.mask = NOTIFICATION_MASK;
    schema.factoryKind = ControlFactoryKind::NOTIFICATION_BINDING;
    next->notificationFallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installThemeCatalogSchema(ControlContext &context,
                                                                                         bool recipient) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }
  try {
    auto installed = installThemeSelectorSchema(context);
    if (!installed) {
      return installed;
    }
    if (recipient) {
      const auto view = chainFor(*context.data_, "ThemeSelector", context.limits_, {});
      if (!view) {
        return std::unexpected(view.error());
      }
      if (view->factoryKind != ControlFactoryKind::THEME_SELECTOR_BINDING &&
          view->factoryKind != ControlFactoryKind::THEME_CATALOG_BINDING) {
        return std::unexpected(error(Code::INVALID_VALUE, "ThemeSelector"));
      }
    }
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    if (!hasDeclaredType(context, "ThemeSelectorCatalog") && !fallbackFor(*next, "ThemeSelectorCatalog")) {
      ControlContext::Data::Schema schema;
      schema.name = "ThemeSelectorCatalog";
      schema.mask = THEME_CATALOG_MASK;
      schema.factoryKind = ControlFactoryKind::THEME_CATALOG_BINDING;
      next->catalogFallback.emplace(std::move(schema));
    }
    next->catalogRecipient = next->catalogRecipient || recipient;
    context.data_ = std::move(next);
    context.prepared_ = false;
    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::themeFamilySchema(ControlContext &context,
                                                                                 std::string_view name,
                                                                                 ControlFactoryKind kind) noexcept {
  try {
    auto next = std::make_shared<ControlContext::Data>(*context.data_);
    auto found = std::ranges::find(next->schemas, name, &ControlContext::Data::Schema::name);
    if (found != next->schemas.end()) {
      found->factoryKind = kind;
    }
    context.data_ = std::move(next);
    context.prepared_ = false;
    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError>
ControlContextAccess::installThemeSelectorSchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  constexpr std::string_view name = "ThemeSelector";

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;

    schema.name = name;
    schema.mask = THEME_SELECTOR_MASK;
    schema.factoryKind = ControlFactoryKind::THEME_SELECTOR_BINDING;
    next->selectorFallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installMessageSchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  constexpr std::string_view name = "MessageBox";

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;

    schema.name = name;
    schema.mask = MESSAGE_MASK;
    schema.factoryKind = ControlFactoryKind::MESSAGE_BINDING;
    next->messageFallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installDialogSchema(ControlContext &context,
                                                                                   std::string_view name) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  if (name != "OverlayHost" && name != "Dialog") {
    return std::unexpected(ControlContextError{Code::UNKNOWN_TYPE, {}});
  }

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;

    schema.name = name;
    schema.mask = name == "OverlayHost" ? OVERLAY_HOST_MASK : DIALOG_MASK;
    schema.factoryKind = ControlFactoryKind::DIALOG_FALLBACK;
    next->dialogFallbacks[name == "OverlayHost" ? 0 : 1] = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError> ControlContextAccess::installKeyHelpSchema(ControlContext &context,
                                                                                    std::string_view name) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }
  if (name != "HelpView" && name != "KeyHints") {
    return std::unexpected(ControlContextError{Code::UNKNOWN_TYPE, {}});
  }
  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }
  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;
    schema.name = name;
    schema.mask = name == "HelpView" ? HELP_VIEW_MASK : KEY_HINTS_MASK;
    schema.factoryKind = ControlFactoryKind::KEY_HELP_FALLBACK;
    next->keyHelpFallbacks[name == "HelpView" ? 0 : 1] = std::move(schema);
    context.data_ = std::move(next);
    context.prepared_ = false;
    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<void, ControlContextError>
ControlContextAccess::installVirtualViewSchema(ControlContext &context, std::string_view name) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  if (name != "VirtualList" && name != "VirtualTable" && name != "Chart") {
    return std::unexpected(ControlContextError{Code::UNKNOWN_TYPE, {}});
  }

  if (hasDeclaredType(context, name) || (context.data_ && fallbackFor(*context.data_, name))) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;

    constexpr std::array masks{VIRTUAL_LIST_MASK, VIRTUAL_TABLE_MASK, CHART_MASK};
    const auto slot = name == "VirtualList" ? 0U : name == "VirtualTable" ? 1U : 2U;

    schema.name = name;
    schema.mask = masks[slot];
    schema.factoryKind = ControlFactoryKind::VIRTUAL_VIEW_FALLBACK;
    next->virtualViewFallbacks[slot] = std::move(schema);
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}
} // namespace detail

ControlContext::ControlContext(ControlContextLimits limits) noexcept : limits_(limits) {
  if (!limits.maxDefinitions || !limits.maxInheritance || limits.maxInheritance > MAX_STYLE_BASE_TYPES ||
      !limits.maxStructDepth || !limits.maxMaterializedLeaves) {
    failure_ = ControlContextError{Code::INVALID_LIMIT, {}};
  }
}

ControlContext &ControlContext::registerType(std::string_view name,
                                             std::span<const std::string_view> properties) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    if (!identifier(name)) {
      failure_ = error(Code::INVALID_NAME, name);
    } else if (builtin(name)) {
      failure_ = error(Code::BUILTIN_SHADOW, name);
    } else {
      auto next = data_ ? std::make_shared<Data>(*data_) : std::make_shared<Data>();

      if (definition(*next, name) != next->definitions.end() ||
          std::ranges::find(next->schemas, name, &Data::Schema::name) != next->schemas.end()) {
        failure_ = error(Code::DUPLICATE_TYPE, name);
      } else if (next->definitions.size() + next->schemas.size() >= limits_.maxDefinitions) {
        failure_ = error(Code::DEFINITION_LIMIT, name);
      } else {
        Data::Schema schema;
        schema.name = name;

        for (const auto path : properties) {
          const auto index = propertyIndex(path);

          if (index == PROPERTIES.size()) {
            failure_ = error(Code::UNKNOWN_PROPERTY, name, path);
            break;
          }

          if (schema.mask.test(index)) {
            failure_ = error(Code::DUPLICATE_PROPERTY, name, path);
            break;
          }

          schema.mask.set(index);
        }

        if (!failure_) {
          next->schemas.push_back(schema);
          data_ = std::move(next);
        }
      }
    }
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

ControlContext &ControlContext::define(std::string_view name, std::string_view base,
                                       const ControlSettings &settings) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    const auto checked = validateSettings(settings, name, ALL_MASK, false, limits_);

    if (!checked) {
      failure_ = checked.error();

      return *this;
    }

    if (!identifier(name) || !identifier(base)) {
      failure_ = error(Code::INVALID_NAME, !identifier(name) ? name : base);
    } else if (builtin(name)) {
      failure_ = error(Code::BUILTIN_SHADOW, name);
    } else {
      auto next = data_ ? std::make_shared<Data>(*data_) : std::make_shared<Data>();

      if (definition(*next, name) != next->definitions.end() ||
          std::ranges::find(next->schemas, name, &Data::Schema::name) != next->schemas.end()) {
        failure_ = error(Code::DUPLICATE_TYPE, name);
      } else if (next->definitions.size() + next->schemas.size() >= limits_.maxDefinitions) {
        failure_ = error(Code::DEFINITION_LIMIT, name);
      } else {
        Data::Entry entry;
        entry.name = name;
        entry.base = base;
        entry.settings = settings;
        next->definitions.push_back(entry);
        data_ = std::move(next);
      }
    }
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

ControlContext &ControlContext::defaults(std::string_view type, const ControlSettings &settings) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    const auto checked = validateSettings(settings, type, ALL_MASK, false, limits_);

    if (!checked) {
      failure_ = checked.error();

      return *this;
    }

    if (!identifier(type)) {
      failure_ = error(Code::INVALID_NAME, type);
    } else {
      auto next = data_ ? std::make_shared<Data>(*data_) : std::make_shared<Data>();

      if (std::ranges::find(next->defaults, type, &Data::Entry::name) != next->defaults.end()) {
        failure_ = error(Code::DUPLICATE_PROPERTY, type);
      } else if (next->defaults.size() >= BUILTIN_COUNT &&
                 next->defaults.size() - BUILTIN_COUNT >= limits_.maxDefinitions) {
        failure_ = error(Code::DEFINITION_LIMIT, type);
      } else {
        Data::Entry entry;
        entry.name = type;
        entry.settings = settings;
        next->defaults.push_back(entry);
        data_ = std::move(next);
      }
    }
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

ControlContext &ControlContext::overlayDefaults(std::string_view type, const ControlSettings &settings) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    if (!data_ || std::ranges::find(data_->defaults, type, &Data::Entry::name) == data_->defaults.end()) {
      return defaults(type, settings);
    }

    const auto checked = validateSettings(settings, type, ALL_MASK, false, limits_);

    if (!checked) {
      failure_ = checked.error();

      return *this;
    }

    auto next = std::make_shared<Data>(*data_);
    auto &entry = *std::ranges::find(next->defaults, type, &Data::Entry::name);

    if (mergedLeafCount(entry.settings, settings) > limits_.maxMaterializedLeaves) {
      failure_ = error(Code::MATERIALIZED_LEAF_LIMIT, type);

      return *this;
    }

    merge(entry.settings, settings);
    data_ = std::move(next);
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

std::expected<void, ControlContextError> ControlContext::validate() const noexcept {
  if (failure_) {
    return std::unexpected(*failure_);
  }

  if (prepared_ || !data_) {
    return {};
  }

  try {
    for (const auto &entry : data_->definitions) {
      const auto resolved = resolveData(*data_, entry.name, {}, limits_, entry.settings.origin);

      if (!resolved) {
        return std::unexpected(resolved.error());
      }
    }

    for (const auto &entry : data_->defaults) {
      const auto resolved = resolveData(*data_, entry.name, {}, limits_, entry.settings.origin);

      if (!resolved) {
        return std::unexpected(resolved.error());
      }
    }

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<ControlContext, ControlContextError> ControlContext::prepared() const noexcept {
  const auto checked = validate();
  if (!checked) {
    return std::unexpected(checked.error());
  }
  auto result = *this;
  result.prepared_ = true;
  return result;
}

bool ControlContext::empty() const noexcept {
  const ControlContextLimits defaults;
  return !data_ && !failure_ && limits_.maxDefinitions == defaults.maxDefinitions &&
         limits_.maxInheritance == defaults.maxInheritance && limits_.maxStructDepth == defaults.maxStructDepth &&
         limits_.maxMaterializedLeaves == defaults.maxMaterializedLeaves;
}

bool ControlContext::hasRegisteredType(std::string_view name) const noexcept {
  return data_ && std::ranges::find(data_->schemas, name, &Data::Schema::name) != data_->schemas.end();
}

bool ControlContext::matchesRegisteredType(std::string_view name,
                                           std::span<const std::string_view> properties) const noexcept {
  if (!data_) {
    return false;
  }
  const auto found = std::ranges::find(data_->schemas, name, &Data::Schema::name);
  if (found == data_->schemas.end()) {
    return false;
  }
  Mask mask{};
  for (const auto path : properties) {
    const auto index = propertyIndex(path);
    if (index == PROPERTIES.size() || mask.test(index)) {
      return false;
    }
    mask.set(index);
  }
  return found->mask == mask;
}

std::expected<ResolvedControl, ControlContextError>
ControlContext::resolve(std::string_view name, const ControlSettings &instance) const noexcept {
  const auto checked = validate();

  if (!checked) {
    return std::unexpected(checked.error());
  }

  try {
    if (!identifier(name)) {
      return std::unexpected(error(Code::INVALID_NAME, name));
    }

    if (data_) {
      return resolveData(*data_, name, instance, limits_);
    }

    const Data empty;

    return resolveData(empty, name, instance, limits_);
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

ControlContextLimits ControlContext::limits() const noexcept {
  return limits_;
}
} // namespace portals::ui
