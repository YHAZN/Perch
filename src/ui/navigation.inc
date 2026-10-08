// Part of device_ui.cpp (included there, one translation unit). Screens, back stack, edge drags, Control Center motion.
// ---------- navigation ----------
void refreshDynamic();
void startScan();
void renderScan();
lv_obj_t *layer(Screen s) { return scr[(int)s]; }
void setY(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }
// Motion: ease-out when something arrives, ease-in when it leaves (design/index.html --ease).
void animY(lv_obj_t *o, int from, int to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done) {
  lv_anim_delete(o, setY);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, path);
  lv_anim_set_exec_cb(&a, setY);
  if (done) lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}
// Only the current layer stays visible once motion has finished.
void settleLayers(lv_anim_t * = nullptr) {
  for (int i = 0; i < (int)Screen::Count; ++i) {
    if (i == (int)current) continue;
    lv_anim_delete(scr[i], setY);
    lv_obj_set_y(scr[i], 0);
    lv_obj_add_flag(scr[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_y(layer(current), 0);
}
void onEnter(Screen s);
// Where "back" goes: each screen opened from somewhere remembers it. Home clears it.
std::vector<Screen> backStack;
extern std::vector<std::pair<lv_obj_t *, lv_obj_t *>> screenTitles;
lv_obj_t *backButton = nullptr;
void enter(Screen next) {
  // Automatic Wi-Fi retries would cancel scans; pause them while choosing a network.
  networkPauseRetries(next == Screen::Wifi || next == Screen::Password);
  if (next != Screen::Camera) stopPreview();
  current = next;
  onEnter(next);
  refreshDynamic();
  hide(homeBar, false);  // the bottom line always does something: grid, home, or close
  // The back arrow appears on screens you reached from inside another app (two levels deep).
  const bool showBack = !backStack.empty() && backStack.back() != Screen::Face && backStack.back() != Screen::Apps &&
                        next != Screen::Notice;
  if (backButton) hide(backButton, !showBack);
  for (auto &pair : screenTitles)
    if (pair.first == layer(next)) lv_obj_set_x(pair.second, showBack ? 58 : 22);
  if (next == Screen::Face) backStack.clear();
}
// Opening something: it rises a short distance into place over what was there.
void show(Screen next) {
  if (next == current) {
    enter(next);
    return;
  }
  if (next == Screen::Face) backStack.clear();
  else if (current == Screen::Face || current == Screen::Apps) {
    backStack.clear();
    backStack.push_back(current);
  } else backStack.push_back(current);
  lv_obj_t *to = layer(next);
  lv_obj_remove_flag(to, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(to);
  if (ccOpen) lv_obj_move_foreground(cc);
  enter(next);
  animY(to, 28, 0, 260, lv_anim_path_ease_out, settleLayers);
}
void notice(const String &message, Screen back) {
  lv_label_set_text(noticeText, message.c_str());
  noticeReturn = back;
  show(Screen::Notice);
}
// Going home: the app lifts away and the face is underneath.
void leaveTo(Screen under, int fromY) {
  if (under == Screen::Face) backStack.clear();
  else if (!backStack.empty() && backStack.back() == under) backStack.pop_back();
  lv_obj_t *top = layer(current), *below = layer(under);
  lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(below, 0);
  lv_obj_move_foreground(top);
  enter(under);
  animY(top, fromY, -H, 240, lv_anim_path_ease_in, settleLayers);
}
void openApps(int fromY = H) {
  lv_obj_t *grid = layer(Screen::Apps);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(grid);
  enter(Screen::Apps);
  animY(grid, fromY, 0, 280, lv_anim_path_ease_out, settleLayers);
}
void setX(void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, v); }
void animX(lv_obj_t *o, int from, int to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done) {
  lv_anim_delete(o, setX);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, path);
  lv_anim_set_exec_cb(&a, setX);
  if (done) lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}
void resetX(lv_anim_t *a) {
  for (int i = 0; i < (int)Screen::Count; ++i) lv_obj_set_x(scr[i], 0);
  settleLayers(a);
}
Screen backTarget() { return backStack.empty() ? Screen::Face : backStack.back(); }
// Going back: the screen slides off to the right, revealing where you came from.
void goBack(int fromX = 0) {
  if (current == Screen::Face) return;
  const Screen target = backTarget();
  if (!backStack.empty()) backStack.pop_back();
  lv_obj_t *top = layer(current), *below = layer(target);
  lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(below, 0, 0);
  lv_obj_move_foreground(top);
  enter(target);
  animX(top, fromX, W, 240, lv_anim_path_ease_in, resetX);
}
bool backDragging = false;
void dragBack(int dx) {
  if (!backDragging) {
    backDragging = true;
    lv_obj_t *below = layer(backTarget());
    lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(below, 0, 0);
    lv_obj_move_foreground(layer(current));
  }
  lv_obj_set_x(layer(current), dx);
}
void releaseBack(int dx, int speedX) {
  if (!backDragging) return;
  backDragging = false;
  if (dx > COMMIT_DRAG || (dx > 24 && speedX > 600)) goBack(dx);
  else animX(layer(current), dx, 0, 280, lv_anim_path_ease_out, resetX);
}
// Open an app the way the grid does: its back history starts at the grid.
void launch(Screen app) {
  backStack.clear();
  if (current != Screen::Apps && current != Screen::Face) {
    lv_obj_add_flag(layer(current), LV_OBJ_FLAG_HIDDEN);
    current = Screen::Apps;
  }
  show(app);
}
void goHome() {
  // Same gesture everywhere: from an app to the face, from the face to the app grid.
  if (current == Screen::Face) openApps();
  else leaveTo(Screen::Face, 0);
}

// Interactive home gesture: the layer follows the finger, then commits or springs back.
bool dragging = false;
void dragHome(int dy) {
  if (!dragging) {
    dragging = true;
    if (current == Screen::Face) {
      lv_obj_t *grid = layer(Screen::Apps);
      lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(grid);
    } else {
      lv_obj_t *face = layer(Screen::Face);
      lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_y(face, 0);
      lv_obj_move_foreground(layer(current));
    }
  }
  if (current == Screen::Face) lv_obj_set_y(layer(Screen::Apps), H - dy);
  else lv_obj_set_y(layer(current), -dy);
}
void releaseHome(int dy, int speed) {
  if (!dragging) return;
  dragging = false;
  const bool commit = dy > COMMIT_DRAG || (dy > 24 && speed > 600);
  if (current == Screen::Face) {
    if (commit) openApps(H - dy);
    else animY(layer(Screen::Apps), H - dy, H, 260, lv_anim_path_ease_out, settleLayers);
  } else {
    if (commit) leaveTo(Screen::Face, -dy);
    else animY(layer(current), -dy, 0, 280, lv_anim_path_ease_out, settleLayers);
  }
}

// Control Center: pulled down from the top edge over whatever is on screen.
void ccClosed(lv_anim_t *) {
  lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
  if (topBar) lv_obj_remove_flag(topBar, LV_OBJ_FLAG_HIDDEN);
}
void openControl(int fromY = -H) {
  ccOpen = true;
  if (topBar) lv_obj_add_flag(topBar, LV_OBJ_FLAG_HIDDEN);
  refreshDynamic();
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cc);
  animY(cc, fromY, 0, 280, lv_anim_path_ease_out, nullptr);
}
void closeControl(int fromY = 0) {
  if (!ccOpen) return;
  ccOpen = false;
  animY(cc, fromY, -H, 240, lv_anim_path_ease_in, ccClosed);
}
void dragControl(int dy) {
  if (!ccOpen && !lv_obj_has_flag(cc, LV_OBJ_FLAG_HIDDEN)) return;
  refreshDynamic();
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cc);
  lv_obj_set_y(cc, min(0, -H + dy));
}
void dragControlClosed(int up) {
  if (!ccOpen) return;
  lv_obj_set_y(cc, -max(0, up));
}
void releaseControlClosed(int up, int speed) {
  if (up > COMMIT_DRAG / 2 || (up > 16 && speed > 600)) closeControl(-up);
  else animY(cc, -up, 0, 200, lv_anim_path_ease_out, nullptr);
}
void releaseControl(int dy, int speed) {
  if (dy > COMMIT_DRAG || (dy > 24 && speed < -600)) openControl(-H + dy);
  else {
    ccOpen = true;
    closeControl(-H + dy);
  }
}
