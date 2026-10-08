// Part of device_ui.cpp (included there, one translation unit). Widget helpers: text, circles, pills, toasts, confirm
// sheet.
// ---------- widget helpers ----------
// Plain objects are decoration by default: not clickable, so a tap on an icon's glyph or a
// row's label reaches the button underneath. Anything that reacts to touch opts in.
lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
// Scrolling lists must receive presses on their empty space to scroll.
void scrollable(lv_obj_t *o) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}
lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, value);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}
lv_obj_t *circle(lv_obj_t *parent, int size, lv_color_t border, int width, lv_color_t fill, lv_opa_t fillOpa) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, size, size);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, width, 0);
  lv_obj_set_style_border_color(o, border, 0);
  lv_obj_set_style_bg_color(o, fill, 0);
  lv_obj_set_style_bg_opa(o, fillOpa, 0);
  return o;
}
lv_obj_t *screenBase() {
  lv_obj_t *s = plain(root);
  lv_obj_set_size(s, W, H);
  lv_obj_set_style_bg_color(s, VOID_, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  return s;
}
lv_obj_t *scrimBottom(lv_obj_t *parent, int height) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, lv_pct(100), height);
  lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(o, VOID_, 0);
  lv_obj_set_style_bg_grad_color(o, VOID_, 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_grad_opa(o, 170, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
lv_obj_t *pill(lv_obj_t *parent, const char *label, int width) {
  lv_obj_t *p = plain(parent);
  lv_obj_set_size(p, width, 44);
  lv_obj_set_style_radius(p, 22, 0);
  lv_obj_set_style_bg_color(p, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(p, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_t *l = text(p, label, F_BODY, INK);
  lv_obj_center(l);
  return p;
}
void setupImage(lv_image_dsc_t &dsc, const uint16_t *pixels, int w, int h) {
  memset(&dsc, 0, sizeof(dsc));
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = w;
  dsc.header.h = h;
  dsc.header.stride = w * 2;
  dsc.data_size = w * h * 2;
  dsc.data = reinterpret_cast<const uint8_t *>(pixels);
}
void hide(lv_obj_t *o, bool hidden) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
// Set a label only when the text differs: every set redraws the label's area.
void setText(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}
// Toast: one short line at the top that fades by itself. For confirmations and warnings that
// do not need a decision ("Saved", "Blurry, hold still"). Never for errors that need action.
lv_obj_t *toastBox = nullptr, *toastLabel = nullptr;
void toast(const char *message) {
  if (!toastBox) return;
  lv_label_set_text(toastLabel, message);
  lv_obj_remove_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(toastBox);
  lv_anim_delete(toastBox, nullptr);
  lv_obj_set_style_opa(toastBox, LV_OPA_COVER, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, toastBox);
  lv_anim_set_values(&a, 255, 0);
  lv_anim_set_delay(&a, 1800);
  lv_anim_set_duration(&a, 300);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); });
  lv_anim_set_completed_cb(&a, [](lv_anim_t *x) { lv_obj_add_flag((lv_obj_t *)x->var, LV_OBJ_FLAG_HIDDEN); });
  lv_anim_start(&a);
}
// Confirmation sheet for destructive actions: reached by holding something, confirmed by a
// tap on the red action. Tapping outside cancels.
lv_obj_t *confirmLayer = nullptr, *confirmTitle = nullptr, *confirmAction = nullptr;
void (*confirmFn)(uint32_t) = nullptr;
uint32_t confirmArg = 0;
void confirm(const char *title, const char *action, void (*fn)(uint32_t), uint32_t arg) {
  if (!confirmLayer) return;
  lv_label_set_text(confirmTitle, title);
  lv_label_set_text(confirmAction, action);
  confirmFn = fn;
  confirmArg = arg;
  lv_obj_remove_flag(confirmLayer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(confirmLayer);
}
void onClick(lv_obj_t *o, void (*fn)()) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      o, [](lv_event_t *e) { ((void (*)())lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, (void *)fn);
}
lv_obj_t *pressedFeedback(lv_obj_t *o) {
  lv_obj_set_style_opa(o, LV_OPA_60, LV_STATE_PRESSED);
  return o;
}
// Compact pill for secondary actions on top of a photo: 34 px tall, 46 px touch target.
lv_obj_t *chip(lv_obj_t *parent, const char *label) {
  lv_obj_t *c = pressedFeedback(plain(parent));
  lv_obj_set_size(c, LV_SIZE_CONTENT, 34);
  lv_obj_set_style_pad_hor(c, 14, 0);
  lv_obj_set_style_radius(c, 17, 0);
  lv_obj_set_style_bg_color(c, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(c, 230, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  text(c, label, F_SMALL, INK);
  lv_obj_set_ext_click_area(c, 6);
  return c;
}
String ago(uint32_t when) {
  const time_t now = time(nullptr);
  if (!when || !clockKnown() || now < (time_t)when) return "";
  const uint32_t d = now - when;
  if (d < 60) return "just now";
  if (d < 3600) return String(d / 60) + " min ago";
  if (d < 86400) return String(d / 3600) + " h ago";
  char buf[16];
  const time_t t = when;
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%b %e", &tm);
  return buf;
}
