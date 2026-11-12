// --- the IME composition channel (G1.2) ---------------------------------------

TEST_CASE("an IME composition rides the line: preedit maps, the keyboard "
          "defers, the commit lands whole") {
    namespace keys = ws::input::keys;
    using ws::ui::gui::TextInputResult;
    using ws::ui::gui::TextInputState;

    ws::ui::gui_test::stub.reset();
    GuiContext gui;
    char buf[16] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("ime-field");

    const auto run = [&](InputFrame frame) -> TextInputResult {
        gui.begin_frame(frame);
        const TextInputResult out = ws::ui::gui::text_input_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), edit,
            kDefaultChrome);
        gui.end_frame();
        return out;
    };

    // Focus by press, the release settling it - the original suite's
    // own shape (an unlifted press keeps the gesture captured, and the
    // numeric field below wants a press of its own later). Then type
    // "ab" - the line the composition rides.
    run(snap(20.0f, 12.0f, true, true));
    run(snap(60.0f, 12.0f, false, false, true));
    InputFrame typing = snap(-100.0f, -100.0f);
    typing.text_input = {'a', 'b'};
    const TextInputResult typed = run(typing);
    CHECK(typed.edited);
    CHECK(std::strcmp(buf, "ab") == 0);
    CHECK(edit.caret == 2);

    // The composition opens: the preedit rides the line AT the caret
    // (x 24 = content 12 + two fontless chars), the run's own caret
    // stands inside it, and nothing landed in the buffer - the frame
    // answers unedited.
    InputFrame comp = snap(-100.0f, -100.0f);
    comp.composition_begin("c", 1, 1);
    const TextInputResult begun = run(comp);
    CHECK(!begun.edited);
    CHECK(std::strcmp(buf, "ab") == 0);
    REQUIRE(begun.preedit_text != nullptr);
    CHECK(std::strcmp(begun.preedit_text, "c") == 0);
    CHECK(begun.preedit_len == 1);
    CHECK(begun.preedit_at == 2);
    CHECK(begun.preedit_caret_lo == 1);
    CHECK(begun.preedit_x == 24.0f);
    CHECK(begun.preedit_advance == 6.0f);
    CHECK(begun.preedit_caret_x == 30.0f);
    CHECK(begun.caret_x == 30.0f);  // the composition caret is THE caret
    CHECK(begun.blink == 0.0f);     // composing is work: solid

    // The keyboard is the IME's while the composition stands: arrows,
    // deleters and stray glyphs all defer (its arrows walk the
    // candidate list; its glyphs ARE the preedit). One frame carrying
    // every one of them changes nothing.
    InputFrame ime_keys = snap(-100.0f, -100.0f);
    ime_keys.composition_update("c", 1, 1);
    ime_keys.key_pressed[static_cast<std::size_t>(keys::kLeft)] = true;
    ime_keys.key_pressed[static_cast<std::size_t>(keys::kBackspace)] = true;
    ime_keys.text_input = {'z'};
    const TextInputResult deferred = run(ime_keys);
    CHECK(!deferred.edited);
    CHECK(std::strcmp(buf, "ab") == 0);
    CHECK(edit.caret == 2);
    CHECK(std::strcmp(deferred.preedit_text, "c") == 0);

    // A multi-byte re-spell measures per CODEPOINT, clusters whole:
    // "nié" is three codepoints (18 px fontless), the caret span in
    // BYTES (past 'é' is byte 4).
    InputFrame respell = snap(-100.0f, -100.0f);
    respell.composition_update("ni\xc3\xa9", 4, 4);
    const TextInputResult grown = run(respell);
    CHECK(!grown.edited);
    CHECK(std::strcmp(buf, "ab") == 0);
    CHECK(grown.preedit_advance == 18.0f);
    CHECK(grown.preedit_x == 24.0f);
    CHECK(grown.preedit_caret_x == 42.0f);  // 24 + the whole run

    // The commit: a whole UTF-8 span through the field's filter -
    // CJK lands as its bytes, whole, the caret past it.
    InputFrame commit = snap(-100.0f, -100.0f);
    commit.composition_end("\xe4\xbd\xa0\xe5\xa5\xbd");  // "你好"
    const TextInputResult landed = run(commit);
    CHECK(landed.edited);
    CHECK(std::strcmp(buf, "ab\xe4\xbd\xa0\xe5\xa5\xbd") == 0);
    CHECK(edit.caret == 8);
    CHECK(landed.preedit_text == nullptr);  // the run is gone
    CHECK(landed.caret_x == 36.0f);  // 12 + four fontless codepoints

    // A commit over a standing selection replaces it - the typing
    // rule. Select all, then commit "XY".
    InputFrame all = snap(-100.0f, -100.0f);
    all.key_down[static_cast<std::size_t>(keys::kLeftControl)] = true;
    all.key_pressed[static_cast<std::size_t>(keys::kA)] = true;
    run(all);
    CHECK(edit.anchor == 0);
    CHECK(edit.caret == 8);
    InputFrame over = snap(-100.0f, -100.0f);
    over.composition_end("XY");
    const TextInputResult replaced = run(over);
    CHECK(replaced.edited);
    CHECK(std::strcmp(buf, "XY") == 0);

    // The numeric field's filter applies to a commit exactly as to a
    // paste: the digits land, the letters drop, a pure-CJK commit is
    // refused whole.
    char num[16] = "";
    TextInputState num_edit;
    const ws::ui::gui::WidgetId num_id = gui.ids().id("ime-num");
    const auto run_num = [&](InputFrame frame) -> bool {
        gui.begin_frame(frame);
        const TextInputResult out = ws::ui::gui::text_input_behavior(
            gui, num_id, rect, num, static_cast<int>(sizeof num), num_edit,
            kDefaultChrome, false, {},
            &ws::ui::gui::is_float_insertable);
        gui.end_frame();
        return out.edited;
    };
    InputFrame num_press = snap(20.0f, 12.0f, true, true);
    run_num(num_press);
    InputFrame num_commit = snap(-100.0f, -100.0f);
    num_commit.composition_end("3x5");
    CHECK(run_num(num_commit));
    CHECK(std::strcmp(num, "35") == 0);
    InputFrame cjk_commit = snap(-100.0f, -100.0f);
    cjk_commit.composition_end("\xe4\xbd\xa0\xe5\xa5\xbd");
    CHECK(!run_num(cjk_commit));
    CHECK(std::strcmp(num, "35") == 0);

    // An unfocused field neither shows nor consumes the frame's
    // composition: the channel is global, the consumption is the
    // focused field's.
    char other[16] = "";
    TextInputState other_edit;
    const ws::ui::gui::WidgetId other_id = gui.ids().id("ime-other");
    InputFrame live = snap(-100.0f, -100.0f);
    live.composition_begin("q", 1, 1);
    gui.begin_frame(live);
    const TextInputResult bystander = ws::ui::gui::text_input_behavior(
        gui, other_id, rect, other, static_cast<int>(sizeof other),
        other_edit, kDefaultChrome);
    gui.end_frame();
    CHECK(bystander.preedit_text == nullptr);
    CHECK(!bystander.edited);
    live.composition_end("q");
    gui.begin_frame(live);
    (void)ws::ui::gui::text_input_behavior(
        gui, other_id, rect, other, static_cast<int>(sizeof other),
        other_edit, kDefaultChrome);
    gui.end_frame();
    CHECK(std::strcmp(other, "") == 0);  // the focus was elsewhere

    // A dead field (no room) rides a composition and a commit without
    // a crash and without pretending to edit.
    TextInputState dead_edit;
    const ws::ui::gui::WidgetId dead_id = gui.ids().id("ime-dead");
    InputFrame dead_frame = snap(-100.0f, -100.0f);
    dead_frame.composition_begin("x", 1, 1);
    dead_frame.composition_end("yz");
    gui.begin_frame(dead_frame);
    const TextInputResult dead = ws::ui::gui::text_input_behavior(
        gui, dead_id, rect, nullptr, 1, dead_edit, kDefaultChrome);
    gui.end_frame();
    CHECK(!dead.edited);
    CHECK(dead.preedit_text == nullptr);
}

TEST_CASE("the composing field draws the spliced line, the underline and a "
          "solid composition caret") {
    using ws::ui::gui::TextInputState;
    using ws::ui::r2d::Font;

    // 95 ASCII glyphs at 10 px - the fit_text test's own font shape.
    std::array<Font::Glyph, 95> glyphs{};
    for (Font::Glyph& glyph : glyphs) {
        glyph.advance = 10.0f;
    }
    const Font font(16.0f, 20.0f, glyphs.data(), 95);
    ws::ui::gui::ChromeTheme themed = kDefaultChrome;
    themed.font = &font;

    GuiContext gui;
    char buf[16] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{0.0f, 0.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("ime-draw");

    const auto run = [&](InputFrame frame) -> ws::ui::gui::TextInputResult {
        gui.begin_frame(frame);
        const ws::ui::gui::TextInputResult out =
            ws::ui::gui::text_input_behavior(
                gui, id, rect, buf, static_cast<int>(sizeof buf), edit,
                themed);
        ws::ui::gui::text_input_draw(rect, out, themed, "label", buf,
                                     static_cast<int>(sizeof buf));
        gui.end_frame();
        return out;
    };

    run(snap(20.0f, 12.0f, true, true));
    InputFrame typing = snap(-100.0f, -100.0f);
    typing.text_input = {'a', 'b'};
    (void)run(typing);
    CHECK(std::strcmp(buf, "ab") == 0);

    // The composition mid-line: a press at x 18 places the caret at
    // byte 1, the preedit "X" rides BETWEEN 'a' and 'b', and the stock
    // draw spells the spliced line "aXb" as ONE text run - ring, face,
    // underline, solid composition caret, no blink caret.
    gui.begin_frame(snap(18.0f, 12.0f, true, true));
    (void)ws::ui::gui::text_input_behavior(
        gui, id, rect, buf, static_cast<int>(sizeof buf), edit, themed);
    gui.end_frame();
    CHECK(edit.caret == 1);

    ws::ui::gui_test::stub.reset();
    InputFrame comp = snap(-100.0f, -100.0f);
    comp.composition_begin("X", 1, 1);
    const ws::ui::gui::TextInputResult composing = run(comp);
    CHECK(composing.preedit_x == 22.0f);  // 12 + one 10px glyph
    CHECK(composing.preedit_advance == 10.0f);
    CHECK(composing.preedit_caret_x == 32.0f);
    CHECK(ws::ui::gui_test::stub.rounded == 2);  // the ring + the face
    CHECK(ws::ui::gui_test::stub.texts == 1);    // ONE spliced text run
    CHECK(ws::ui::gui_test::stub.last_text == "aXb");
    CHECK(ws::ui::gui_test::stub.quads == 2);    // underline + caret
    CHECK(ws::ui::gui_test::stub.last_quad_color == themed.text);

    // The fontless stock draws the same geometry as bars: content bar,
    // underline, composition caret.
    ws::ui::gui_test::stub.reset();
    gui.begin_frame(comp);
    const ws::ui::gui::TextInputResult bare = ws::ui::gui::text_input_behavior(
        gui, id, rect, buf, static_cast<int>(sizeof buf), edit,
        kDefaultChrome);
    ws::ui::gui::text_input_draw(rect, bare, kDefaultChrome, "label", buf,
                                 static_cast<int>(sizeof buf));
    gui.end_frame();
    CHECK(bare.preedit_x == 18.0f);  // 12 + one 6px fontless char
    CHECK(bare.preedit_advance == 6.0f);
    CHECK(ws::ui::gui_test::stub.texts == 0);
    CHECK(ws::ui::gui_test::stub.quads == 3);  // bar + underline + caret
}

TEST_CASE("the IME rect seam forwards the focused field's bounds on change") {
    using ws::ui::gui::TextInputState;

    GuiContext gui;
    char buf[16] = "";
    TextInputState edit;
    const ws::ui::gui::Rect rect{10.0f, 20.0f, 176.0f, 24.0f};
    const ws::ui::gui::WidgetId id = gui.ids().id("ime-rect");

    std::vector<ws::ui::gui::Rect> forwarded;
    gui.set_ime_rect(
        [&forwarded](const ws::ui::gui::Rect& r) { forwarded.push_back(r); });

    const auto run = [&](InputFrame frame) {
        gui.begin_frame(frame);
        (void)ws::ui::gui::text_input_behavior(
            gui, id, rect, buf, static_cast<int>(sizeof buf), edit,
            kDefaultChrome);
        gui.end_frame();
    };

    // Focus gain forwards the field's bounds exactly once - on the
    // frame AFTER the claiming press (state() latches the focused rect
    // as the widget runs, before the press claim lands; the header's
    // documented one-frame lag).
    run(snap(20.0f, 32.0f, true, true));
    CHECK(forwarded.empty());  // the claiming frame itself: no latch yet
    run(snap(60.0f, 32.0f, false, false, true));  // the release settles it
    REQUIRE(forwarded.size() == 1);
    CHECK(forwarded[0].x == rect.x);
    CHECK(forwarded[0].y == rect.y);
    CHECK(forwarded[0].width == rect.width);
    CHECK(forwarded[0].height == rect.height);

    // ...and quiet frames echo nothing: the seam hears moves, not
    // every frame's repetition.
    run(snap(-100.0f, -100.0f));
    run(snap(-100.0f, -100.0f));
    CHECK(forwarded.size() == 1);

    // Focus loss (a press elsewhere) forwards the zero rect once -
    // "no field holds focus, hide the composition window" - and then
    // keeps its silence.
    run(snap(-100.0f, -100.0f, true, true));
    REQUIRE(forwarded.size() == 2);
    CHECK(forwarded[1].x == 0.0f);
    CHECK(forwarded[1].width == 0.0f);
    run(snap(-100.0f, -100.0f));
    CHECK(forwarded.size() == 2);

    // A context with no seam installed runs the same frames without a
    // crash - the unset seam is the headless answer.
    GuiContext bare;
    TextInputState bare_edit;
    bare.begin_frame(snap(20.0f, 32.0f, true, true));
    (void)ws::ui::gui::text_input_behavior(
        bare, bare.ids().id("bare"), rect, buf,
        static_cast<int>(sizeof buf), bare_edit, kDefaultChrome);
    bare.end_frame();
}