/* Figo phone control interface.
 *
 * Vanilla, no build step, no dependencies - it is served off the device's own
 * LittleFS in SoftAP mode, where there is no internet to fetch anything from.
 *
 * Rendering strategy: index.html carries the whole page as static markup and
 * this file only fills values in and rebuilds the list-shaped parts (the
 * filmstrip, the dot row, the network list, the freshness rows). A full
 * re-render on every 10s poll would drop input focus mid-typing and reset the
 * strip's scroll, so it is deliberately avoided.
 */
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);

  function el(tag, cls, text) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  // ---------------------------------------------------------------- API --

  async function apiGet(path) {
    const res = await fetch(path);
    if (!res.ok) {
      throw new Error((await res.json().catch(() => ({}))).error || res.statusText);
    }
    return res.json();
  }

  async function apiPost(path, body) {
    const res = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {}),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || res.statusText);
    return data;
  }

  // Confirmations clear after 2.8s, per the design. Errors do NOT: they
  // usually need acting on, and a message that vanishes before it is read is
  // worse than no message.
  const msgTimers = {};
  function flash(id, text, isError) {
    const node = $(id);
    node.textContent = text;
    node.classList.toggle("err", !!isError);
    clearTimeout(msgTimers[id]);
    if (!isError && text) {
      msgTimers[id] = setTimeout(() => {
        node.textContent = "";
      }, 2800);
    }
  }

  // --------------------------------------------- rules the firmware owns --
  // Everything in this block mirrors a constraint that lives in the firmware.
  // Re-deriving these by eye is how the two drift apart, so each one names the
  // symbol it mirrors.

  // Mirrors screenLayoutValidFor() (SharedState.h): ticker types choose
  // Single/Dual, FEAR_GREED chooses between its two gauge styles.
  const LAYOUTS_BY_TYPE = {
    CRYPTO: [["SINGLE", "Single"], ["DUAL", "Dual ticker"]],
    STOCK: [["SINGLE", "Single"], ["DUAL", "Dual ticker"]],
    COMMODITY: [["SINGLE", "Single"], ["DUAL", "Dual ticker"]],
    FEAR_GREED: [["SLIDER", "Slider"], ["CIRCULAR", "Circular"]],
  };

  // Mirrors screenNameMaxLen(): a dual screen splits the 320px panel into two
  // columns, so each name gets far less room than a full-width title.
  const NAME_MAX = { DUAL: 10, DEFAULT: 12 };

  // MAX_SCREENS (Config.h). The cap has to be enforced HERE because the
  // device does not enforce it in a way anyone would notice: handlePostConfig()
  // `break`s out of the screens loop at the eighth entry rather than erroring,
  // so a ninth screen saves with a cheerful 200 and then silently does not
  // exist. It is also the ceiling on a screen id - expression_source.screen_id
  // is validated as 0 or 1..MAX_SCREENS - which is what bounds assignIds().
  const MAX_SCREENS = 8;

  // SCREEN_INTERVAL_SEC_MIN / _DUAL (Config.h). These are a TLS-session
  // budget, not politeness: a dual screen makes two sessions per refresh and
  // so gets double the floor.
  const INTERVAL_MIN = 60;
  const INTERVAL_MIN_DUAL = 120;

  // What a new screen starts on. The interval floor (INTERVAL_MIN) is a TLS
  // budget, not a recommendation - a quarter of an hour is the sane default for
  // a desk. The symbol matters more than it looks: a new screen that already
  // shows something is the same sheet as an existing one, so there is no
  // add-specific empty state to design, explain or test.
  const INTERVAL_DEFAULT = 900;
  const DEFAULT_SYMBOL = "^GSPC";
  const DEFAULT_NAME = "S&P 500";

  const DIM_GREY = "#787878"; // figo_theme::dimGrey()
  const UP = "#00BC16";
  const DOWN = "#D60307";

  // figo_theme::appendCurrency(). The code comes from the provider via
  // /api/screens/state, never guessed from the symbol. USD gets the design's
  // tight "$", ILS/ILA (Tel Aviv, quoted in agorot) gets nothing, and every
  // other currency gets its ISO code as text - the panel has no euro, pound
  // or yen GLYPH to draw, so the phone shows the same thing it does.
  function currencySuffix(code) {
    const c = String(code || "");
    if (!c) return "";
    if (c.toUpperCase() === "USD") return "$";
    if (c.toUpperCase() === "ILS" || c.toUpperCase() === "ILA") return "";
    return " " + c;
  }

  // Mirrors figo_theme::formatPrice(). Grouping is done by hand rather than
  // with toLocaleString() so the preview matches the panel byte for byte on
  // any phone locale.
  function formatPrice(p, code) {
    const cur = currencySuffix(code);
    if (p >= 1000) {
      const s = String(Math.round(p));
      let out = "";
      for (let i = 0; i < s.length; i++) {
        if (i > 0 && (s.length - i) % 3 === 0) out += ",";
        out += s[i];
      }
      return out + cur;
    }
    if (p >= 1) return p.toFixed(2) + cur;
    return p.toFixed(4) + cur;
  }

  // figo_theme::formatChangePct() - "%+.2f%%"
  function formatChange(v) {
    return (v >= 0 ? "+" : "") + v.toFixed(2) + "%";
  }

  // figo_theme::fearGreedBandColor() / fearGreedRating(). The bounds are the
  // firmware's own (< 25, < 45, <= 55, <= 75), not re-spelled.
  function fgColor(v) {
    if (v < 25) return DOWN;
    if (v < 45) return "#D68503";
    if (v <= 55) return "#C9AE00";
    if (v <= 75) return "#5CCD4D";
    return UP;
  }
  function fgRating(v) {
    if (v < 25) return "EXTREME FEAR";
    if (v < 45) return "FEAR";
    if (v <= 55) return "NEUTRAL";
    if (v <= 75) return "GREED";
    return "EXTREME GREED";
  }

  // ------------------------------------------------- filmstrip geometry --
  // 256px frame + 14px gap. The drag target index and the dot-jump both
  // divide by this, so it must stay in step with .cell/.strip in style.css.
  const PITCH = 270;
  const STRIP_PAD = 22;
  const FRAME_W = 256;

  // Edge auto-scroll while a frame is held. Speed is proportional to how deep
  // into the zone the finger is - a nudge to creep, a bury to fly. Both are
  // first guesses at iOS feel and are expected to want tuning on real glass.
  const EDGE_ZONE = 56;   // px from each edge of the strip's visible box
  const MAX_SPEED = 700;  // px/s at the very edge

  const INTERVAL_CHOICES = [60, 300, 900, 1800];
  const INTERVAL_CHOICES_DUAL = [120, 300, 900, 1800];

  // -------------------------------------------------------------- state --

  const state = {
    tab: "status",
    mode: "AP", // STA | AP | NO_WIFI, from GET /api/status
    status: {},
    // Each: { srvId, type, symbol, name, interval, layout, symbol2, name2 }.
    // srvId is the id the device last told us about, kept so live values can
    // be matched to the right row; null for a screen added but not yet saved.
    // It travels WITH the object through a reorder, which is the whole point:
    // the value belongs to the instrument, not to the slot.
    screens: [],
    values: {}, // srvId -> { fetched, age_s, stale, price, change_pct, ... }
    mood: null,
    sel: 0,
    sheetOpen: false,
    // dragIdx is the liveness flag: null means no gesture is live, and it is
    // what refresh(), renderStrip() and the cell click guard check. dragFrom
    // is the pickup index the transform arithmetic in applyDragVisuals()
    // actually does math against. They are set together and always equal,
    // but they answer different questions - keep both rather than collapsing
    // them into one.
    dragIdx: null,
    dragPointerId: null,
    // Transform-driven reorder. The DOM is frozen for the whole gesture and
    // state.screens is committed once, on drop - see the design spec section 5.
    // grabPx and every derived offset are CONTENT-space, not viewport: that is
    // what keeps the frame under the finger while the strip auto-scrolls.
    dragFrom: -1,
    dragTo: -1,
    grabPx: 0,
    lastClientX: 0,
    autoRaf: 0,    // requestAnimationFrame handle; 0 when the loop is idle
    autoLastT: 0,  // previous frame timestamp, for delta-timed scrolling
    // Captured once per gesture, at pickup (see onFrameDown's hold-timer
    // callback) - NOT re-read from strip.scrollWidth every frame like
    // autoLastT is, and not measured lazily on first auto-scroll either.
    // dx is exactly 0 at pickup, so the held cell's translateX hasn't yet
    // grown past the point where it would visually poke out of the strip's
    // un-transformed content box; once it does, Chromium folds that ink
    // overflow into the strip's own scrollWidth, which keeps inflating for
    // as long as the held cell keeps sliding further right, so a live
    // scrollWidth - clientWidth read (or one taken any later than pickup)
    // chases a target that runs away from it and never actually halts.
    // Cleared back to 0 in commitDrag() when the gesture ends.
    autoScrollMax: 0,
    holdTimer: null,
    // Guards startDrop() against re-entry: dragIdx stays non-null for the
    // whole 180ms flight, so a stray pointerup landing on the strip mid-
    // flight would otherwise call startDrop() again and stack a second,
    // wasted commitDrag() behind the first one.
    dropTimer: 0,
    // Where the finger landed for a pending pick-up, so a finger that sets
    // off to scroll can cancel the hold. Browsing itself is native again -
    // the strip pans, flings and snaps on the compositor (see the touchmove
    // guard in wire()) - so these two are the whole of JS's interest in a
    // touch until a frame is actually held.
    holdStartX: 0,
    holdStartY: 0,
    // Set for 60ms after a drop so the click that follows the release does
    // not open the editor for whatever frame landed under the finger.
    justDragged: false,
    // The star, held as an INDEX rather than an id: ids are reassigned by list
    // position on every save, so an index is the only thing that stays true
    // across an unsaved reorder. -1 means nothing is starred.
    starIdx: -1,
    // Three cut points, four bands - mirrors EXPRESSION_*_DEFAULT in Config.h.
    // Neutral is not here because it is not configured: it is the gap left
    // between happy and bear.
    bull: 5.0,
    happy: 1.0,
    bear: -2.5,
    brightness: 80,
    rotation: 0,
    sleepOn: false,
    sleepStart: "23:00",
    sleepEnd: "07:00",
    networks: [],
    scanning: false,
    cgKeyDirty: false,
    adv: false,
    // Factory reset's two-step arm. resetArmed/resetTimer are only ever set
    // and cleared together, by armReset()/disarmReset().
    resetArmed: false,
    resetTimer: null,
    // Kept so showResetCurtain() can stop the 10s poll: over AP the portal
    // returns at the same address and the page would quietly start repainting
    // itself with wiped data behind the curtain.
    pollTimer: null,
  };

  // -------------------------------------------------------------- theme --
  // A client-side preference, never device config: it themes this page only.

  function applyTheme(bright) {
    document.documentElement.setAttribute("data-theme", bright ? "bright" : "dark");
    $("brightToggle").setAttribute("aria-checked", bright ? "true" : "false");
  }

  function loadTheme() {
    let bright = false;
    try {
      bright = localStorage.getItem("figo-bright") === "1";
    } catch (e) {
      // Private mode / storage disabled - dark is the default anyway.
    }
    applyTheme(bright);
  }

  // ------------------------------------------------------------ helpers --

  function screenAt(i) {
    return state.screens[i];
  }

  function valueFor(sc) {
    return previewValue(sc);
  }

  // What this screen can show RIGHT NOW, from two sources: the device's last
  // poll, and any leg the search has just priced. Two rules, both the
  // firmware's own:
  //   * A pair is all-or-nothing - fetchScreen() only publishes price2 when
  //     both legs succeeded, so a half-filled pair renders "--" on BOTH sides.
  //   * A just-checked leg outranks the stored poll value, which still belongs
  //     to whatever this screen showed before the edit.
  function previewValue(sc) {
    if (!sc) return null;
    const saved = sc.srvId != null ? state.values[sc.srvId] : null;
    const dual = isDual(sc);
    // A leg falls back to the saved poll only when this session hasn't
    // touched it - sc.check1/check2 unset. A leg with ANY flag ("running",
    // "failed", "interrupted", "error" or "superseded") must NOT inherit the
    // saved value: that number still describes whatever instrument (or kind)
    // this screen held before the edit, not the one on screen now.
    // setScreenKind() sets "superseded" on
    // its two sc.symbol-reassigning paths for exactly this reason - the
    // saved poll is keyed by srvId, not by symbol/kind, so it keeps
    // answering for the OLD instrument until the next save.
    const fromSaved1 = saved && saved.fetched && !sc.check1;
    const fromSaved2 = saved && saved.fetched && !sc.check2;
    const l1 = sc.px1
      ? sc.px1
      : fromSaved1
        ? { price: saved.price, change_pct: saved.change_pct, currency: saved.currency }
        : null;
    const l2 = !dual
      ? null
      : sc.px2
        ? sc.px2
        : fromSaved2 && saved.price2 != null
          ? { price: saved.price2, change_pct: saved.change_pct2, currency: saved.currency2 }
          : null;

    if (!l1 || (dual && !l2)) {
      // Keep the freshness flags so the caption still reads true; only the
      // numbers are withheld.
      return saved ? Object.assign({}, saved, { fetched: false }) : null;
    }
    // Staleness describes the SAVED poll, not a leg this session just priced.
    // Report it only for the legs that actually came from the saved poll:
    // fully fresh (every rendered leg just checked) reads as fresh, and a
    // mixed dual (one leg fresh, the other still on the saved poll) keeps the
    // saved staleness rather than laundering an old number under a
    // just-checked caption.
    const usedSaved = (!sc.px1 && fromSaved1) || (dual && !sc.px2 && fromSaved2);
    // usedSaved is only ever true via fromSaved1/fromSaved2, both of which
    // already require `saved` to be truthy - so the null case below is dead;
    // written as a plain read, not a ternary, so it doesn't read as if that
    // case were live.
    const v = {
      fetched: true,
      stale: usedSaved ? !!saved.stale : false,
      age_s: usedSaved ? saved.age_s : 0,
      price: l1.price,
      change_pct: l1.change_pct,
      currency: l1.currency,
    };
    if (dual) {
      v.price2 = l2.price;
      v.change_pct2 = l2.change_pct;
      v.currency2 = l2.currency;
    }
    return v;
  }

  function isDual(sc) {
    return sc.type !== "FEAR_GREED" && sc.layout === "DUAL";
  }

  function nameMaxFor(sc) {
    return isDual(sc) ? NAME_MAX.DUAL : NAME_MAX.DEFAULT;
  }

  function intervalFloor(sc) {
    return isDual(sc) ? INTERVAL_MIN_DUAL : INTERVAL_MIN;
  }

  function intervalLabel(n) {
    return n < 120 ? n + "s" : Math.round(n / 60) + " min";
  }

  function agoText(n) {
    if (n < 60) return n + "s ago";
    if (n < 3600) return Math.round(n / 60) + "m ago";
    return Math.round(n / 3600) + "h ago";
  }

  function pct1(v) {
    return (v >= 0 ? "+" : "") + Number(v).toFixed(1) + "%";
  }

  // One place decides what a screen's dot and caption say, so the Figo tab,
  // the filmstrip and the sheet header can never disagree.
  function health(sc) {
    const v = valueFor(sc);
    if (sc.srvId == null) return { kind: "idle", text: "not saved yet" };
    if (!v) return { kind: "idle", text: "waiting for the first fetch" };
    if (!v.fetched) {
      // Stale with nothing ever fetched means the screen has failed
      // FETCH_STALE_AFTER_FAILURES times from cold - worth flagging, not
      // dressing up as "waiting".
      return v.stale
        ? { kind: "stale", text: "not reachable" }
        : { kind: "idle", text: "waiting for the first fetch" };
    }
    if (v.stale) return { kind: "stale", text: "stale · " + agoText(v.age_s) };
    return { kind: "ok", text: "refreshed " + agoText(v.age_s) };
  }

  const HEALTH_FG = { ok: "ok", stale: "stale", idle: "idle" };
  const HEALTH_BG = { ok: "ok-bg", stale: "stale-bg", idle: "idle-bg" };

  // --------------------------------------------------- device previews --
  // Every coordinate below lives in style.css as a class, in the unscaled
  // 320x172 space. Only the things that
  // genuinely depend on data - colours and the two gauge knob positions - are
  // set here. The frames are a mirror of what LVGL draws; keep them that way.

  function add(parent, cls, text) {
    const n = el("div", cls, text);
    parent.appendChild(n);
    return n;
  }

  // Mirrors ScreenBuilders' applyValue()/setPlaceholder() exactly, dimming
  // included: a stale price on the panel goes grey and gains "(stale)".
  function paintTicker(priceEl, chgEl, currency, price, chg, stale, has) {
    // Coerced, not trusted: a screen switched from SINGLE to DUAL in this
    // session has a stored value with no second leg at all, and reaching
    // .toFixed() on undefined would throw and take the whole strip down.
    price = Number(price) || 0;
    chg = Number(chg) || 0;
    if (!has) {
      priceEl.textContent = "--";
      priceEl.style.color = DIM_GREY;
      chgEl.textContent = stale ? "unavailable" : "";
      chgEl.style.color = DIM_GREY;
      return;
    }
    priceEl.textContent = formatPrice(price, currency);
    if (stale) {
      chgEl.textContent = formatChange(chg) + " (stale)";
      priceEl.style.color = DIM_GREY;
      chgEl.style.color = DIM_GREY;
      return;
    }
    chgEl.textContent = formatChange(chg);
    const c = chg >= 0 ? UP : DOWN;
    priceEl.style.color = c;
    chgEl.style.color = c;
  }

  // Mirrors ScreenBuilders' fitFont(). A currency code makes a price three
  // characters longer, which at the design's size can overhang the dual
  // divider, so both the panel and this preview step the font down until it
  // fits. The budgets are the firmware's own constants (kSinglePriceMaxW 282,
  // kDualPriceMaxW 148) in the same unscaled 320x172 space.
  //
  // Measured from the live DOM rather than computed: the phone renders these
  // frames in its system font, not Montserrat, so LVGL's advance widths would
  // be the wrong numbers here. That means it must run AFTER the stage is in
  // the document - a detached node has no layout and every width reads 0.
  const PRICE_FITS = [
    { sel: ".s-price", max: 282, sizes: [40, 36, 32, 27] },
    { sel: ".d-price", max: 148, sizes: [28, 22, 18] },
    { sel: ".d-price2", max: 148, sizes: [28, 22, 18] },
  ];
  function fitStagePrices(stage) {
    PRICE_FITS.forEach((f) => {
      const node = stage.querySelector(f.sel);
      if (!node) return;
      for (let i = 0; i < f.sizes.length; i++) {
        node.style.fontSize = f.sizes[i] + "px";
        // Last size wins by default: shrink as far as the ladder goes and
        // then overflow visibly, rather than hiding the price.
        if (node.scrollWidth <= f.max) break;
      }
    });
  }

  function buildStage(sc) {
    const st = el("div", "stage");
    const v = valueFor(sc);
    const has = !!(v && v.fetched);
    const stale = !!(v && v.stale);

    if (sc.type === "FEAR_GREED") {
      const title = sc.symbol === "stocks" ? "FEAR & GREED" : "CRYPTO F&G";
      // With no data the panel parks the marker at 50 and greys everything,
      // rather than inventing a score. Same here.
      const score = has ? Math.round(v.price) : 50;
      const colour = has && !stale ? fgColor(score) : DIM_GREY;
      const valText = has ? String(score) : "--";
      const labText = has ? fgRating(score) + (stale ? " (stale)" : "") : stale ? "unavailable" : "";

      if (sc.layout === "CIRCULAR") {
        add(st, "c-title", title);
        add(st, "c-arc");
        add(st, "c-hole");
        // The knob rides the 300deg arc at radius 64 about (235, 96).
        const a = ((210 + (score / 100) * 300) * Math.PI) / 180;
        const knob = add(st, "c-knob");
        knob.style.left = Math.round(235 + 64 * Math.sin(a) - 11.5) + "px";
        knob.style.top = Math.round(96 - 64 * Math.cos(a) - 11.5) + "px";
        knob.style.background = colour;
        add(st, "c-val", valText).style.color = colour;
        add(st, "c-lab", labText).style.color = colour;
      } else {
        add(st, "g-title", title);
        add(st, "g-track");
        const knobLeft = 33 + (score / 100) * 238;
        const knob = add(st, "g-knob");
        knob.style.left = Math.round(knobLeft) + "px";
        knob.style.background = colour;
        // Score and band read as one centred block under the knob, clamped so
        // it cannot slide off either edge of the 320px frame.
        const block = add(st, "g-block");
        block.style.left = Math.max(6, Math.min(190, Math.round(knobLeft + 10 - 62))) + "px";
        add(block, "v", valText).style.color = colour;
        add(block, "l", labText).style.color = colour;
      }
      return st;
    }

    if (isDual(sc)) {
      add(st, "d-div");
      add(st, "d-name", sc.name);
      const p1 = add(st, "d-price");
      const c1 = add(st, "d-change");
      add(st, "d-today", "TODAY");
      add(st, "d-name2", sc.name2);
      const p2 = add(st, "d-price2");
      const c2 = add(st, "d-chg2");
      add(st, "d-today2", "TODAY");
      paintTicker(p1, c1, has ? v.currency : "", has ? v.price : 0, has ? v.change_pct : 0, stale, has);
      // The pair is fetched all-or-nothing (DataFetcher only writes price2
      // when both legs succeeded), so one "has" covers both columns.
      paintTicker(p2, c2, has ? v.currency2 : "", has ? v.price2 : 0, has ? v.change_pct2 : 0, stale, has);
      return st;
    }

    add(st, "s-name", sc.name);
    add(st, "s-ticker", (sc.symbol || "").replace("^", "").toUpperCase());
    const p = add(st, "s-price");
    const c = add(st, "s-change");
    add(st, "s-today", "TODAY");
    paintTicker(p, c, has ? v.currency : "", has ? v.price : 0, has ? v.change_pct : 0, stale, has);
    return st;
  }

  // ----------------------------------------------------------- filmstrip --

  function subtitleFor(sc) {
    if (sc.type === "FEAR_GREED") {
      return sc.symbol === "stocks" ? "stock index" : "crypto index";
    }
    return sc.type.toLowerCase() + " · " + (sc.symbol || "-");
  }

  function renderStrip() {
    const strip = $("strip");
    // Rebuilding the children resets scrollLeft, which would yank the strip
    // back to the first frame on every poll. Save and restore it.
    const keepScroll = strip.scrollLeft;
    strip.textContent = "";

    state.screens.forEach((sc, i) => {
      const cell = el("div", "cell");
      if (i === state.sel) cell.classList.add("sel");
      if (i === state.dragIdx) cell.classList.add("drag");

      const frame = el("div", "frame");
      const stage = buildStage(sc);
      frame.appendChild(stage);

      // Star lives in the frame wrapper, above the scaled stage - see the
      // note on .star in style.css.
      const star = el("button", "star", i === state.starIdx ? "★" : "☆");
      star.type = "button";
      if (i === state.starIdx) star.classList.add("on");
      star.title =
        i === state.starIdx
          ? "This screen drives Figo's face"
          : "Let this screen drive Figo's face";
      star.setAttribute("aria-pressed", i === state.starIdx ? "true" : "false");
      // The starred star is inert, not removed: something always has to drive
      // the face, so the only way out of starred is to star another screen.
      // Kept focusable and in the tab order - aria-disabled, not disabled - so
      // it still announces itself as the pressed one.
      if (i === state.starIdx) star.setAttribute("aria-disabled", "true");
      star.addEventListener("click", (e) => {
        // Without this the tap falls through to the cell and opens the sheet.
        e.stopPropagation();
        setStar(i);
      });
      cell.appendChild(frame);
      cell.appendChild(star);

      const cap = el("div", "cap-row");
      cap.appendChild(el("span", "pos", String(i + 1)));
      cap.appendChild(el("span", "sub", subtitleFor(sc)));
      const h = health(sc);
      cap.appendChild(el("span", "dot6 " + HEALTH_BG[h.kind]));
      cell.appendChild(cap);

      cell.addEventListener("pointerdown", (e) => onFrameDown(i, e));
      cell.addEventListener("click", () => {
        // A drop must not open the sheet; onStripUp holds this flag for 60ms.
        if (state.justDragged || state.dragIdx !== null) return;
        state.sel = i;
        // The outline means "this is the frame in the middle" (see
        // syncSelToScroll), so a tap on a half-visible neighbour has to bring
        // it to the middle - otherwise the tap strands the outline off-centre,
        // which is exactly the state this centring removes.
        centerFrame(i);
        openSheet();
        renderStrip();
      });
      strip.appendChild(cell);
      // After insertion: fitStagePrices() needs real layout to measure.
      fitStagePrices(stage);
    });

    const full = state.screens.length >= MAX_SCREENS;
    const addCell = el("div", "add-cell");
    const addBtn = el("button", "add-btn", full ? "Max " + MAX_SCREENS : "+ Add");
    addBtn.type = "button";
    // Not `disabled`: a disabled button swallows the click, and then the only
    // feedback for hitting the cap is nothing at all. addScreen() flashes the
    // reason instead.
    if (full) addBtn.classList.add("full");
    addBtn.addEventListener("click", addScreen);
    addCell.appendChild(addBtn);
    strip.appendChild(addCell);

    strip.scrollLeft = keepScroll;
    renderDots();
    $("stripHint").textContent =
      state.dragIdx !== null
        ? "Drop it where you want it."
        : "Tap a frame to edit it, hold and drag to reorder.";
    $("screensTitle").textContent =
      state.screens.length + (state.screens.length === 1 ? " screen" : " screens");
  }

  // The poll path. Replaces just the value-bearing parts of each frame, in
  // place: no cell is destroyed, so scrollLeft is never touched and momentum
  // scrolling on a phone survives a refresh landing mid-flick.
  function refreshStages() {
    const cells = $("strip").querySelectorAll(".cell");
    // A structural change (add, remove, reorder) is not this function's job -
    // fall back to the full rebuild if the two have drifted apart.
    if (cells.length !== state.screens.length) {
      renderStrip();
      return;
    }
    state.screens.forEach((sc, i) => {
      const cell = cells[i];
      const frame = cell.querySelector(".frame");
      const old = frame.querySelector(".stage");
      if (old) {
        const stage = buildStage(sc);
        frame.replaceChild(stage, old);
        fitStagePrices(stage);
      }
      const h = health(sc);
      cell.querySelector(".cap-row .dot6").className = "dot6 " + HEALTH_BG[h.kind];
    });
  }

  function renderDots() {
    const dots = $("dots");
    dots.textContent = "";
    state.screens.forEach((sc, i) => {
      const b = el("button", i === state.sel ? "on" : "");
      b.type = "button";
      b.setAttribute("aria-label", "Go to screen " + (i + 1));
      b.addEventListener("click", () => {
        state.sel = i;
        centerFrame(i);
        renderStrip();
      });
      dots.appendChild(b);
    });
  }

  // Which frame the strip is centred on right now. Pure geometry against the
  // same PITCH/STRIP_PAD/FRAME_W constants centerFrame() and the drag use -
  // deliberately NOT a read of the cells' bounding boxes, which during a drag
  // carry transforms that say nothing about where a frame will land.
  function centeredIndex() {
    const strip = $("strip");
    if (!state.screens.length) return -1;
    const mid = strip.scrollLeft + strip.clientWidth / 2;
    const i = Math.round((mid - STRIP_PAD - FRAME_W / 2) / PITCH);
    return Math.max(0, Math.min(state.screens.length - 1, i));
  }

  // The white outline marks the frame in the middle of the strip. It cannot
  // mark whichever frame was tapped last, which after any swipe would simply
  // be some other screen. state.sel is that frame AND the one the editor sheet
  // edits, so the two are kept in step from both ends: a scroll moves sel
  // here, and a tap centres the frame it opens.
  //
  // Classes only - never renderStrip(). A rebuild mid-fling resets scrollLeft
  // and kills the glide, which is the same trap the poll path avoids with
  // refreshStages().
  function syncSelToScroll() {
    // The DOM is frozen for a whole reorder gesture, and auto-scroll writes
    // scrollLeft on every frame of it; sel is committed by moveScreen().
    if (state.dragIdx !== null) return;
    // The sheet is a modal over the strip, and it binds against state.sel:
    // moving sel out from under an open sheet would have the fields writing
    // into a screen other than the one on show.
    if (state.sheetOpen) return;
    const i = centeredIndex();
    if (i < 0 || i === state.sel) return;
    state.sel = i;
    paintSel();
  }

  // Repaints just what sel controls, in place. Mirrors the two places
  // renderStrip() writes the same thing (the .sel class, the dot's .on).
  function paintSel() {
    const cells = $("strip").querySelectorAll(".cell");
    cells.forEach((c, i) => c.classList.toggle("sel", i === state.sel));
    const dots = $("dots").children;
    for (let i = 0; i < dots.length; i++) {
      dots[i].className = i === state.sel ? "on" : "";
    }
  }

  function centerFrame(i) {
    const strip = $("strip");
    const left = STRIP_PAD + i * PITCH + FRAME_W / 2 - strip.clientWidth / 2;
    // behavior:'auto' on purpose: with scroll-snap-type:x mandatory the engine
    // silently cancels programmatic smooth scrolling and nothing moves.
    strip.scrollTo({ left: Math.max(0, left), behavior: "auto" });
  }

  // --------------------------------------------------- drag to reorder --

  // Viewport x -> strip content x. Everything the drag measures lives in this
  // space, because content x is invariant under scrolling: when the strip
  // scrolls right by S with a stationary finger, scrollLeft grows by S, so
  // this grows by S, so the held cell's translate grows by S and it stays put
  // under the finger. Viewport coordinates would slide it away exactly when
  // auto-scroll is running.
  function stripContentX(clientX) {
    const strip = $("strip");
    return clientX - strip.getBoundingClientRect().left + strip.scrollLeft;
  }

  // Recomputes the target slot and repaints every cell transform. Called from
  // pointermove AND from each auto-scroll frame - during auto-scroll the finger
  // is stationary while the content moves, so the target genuinely changes
  // without any pointer event to prompt it.
  function applyDragVisuals() {
    // Only meaningful while a frame is actually held. Without this an rAF
    // frame that lands after the drop writes translateX(-PITCH) onto cell 0
    // and leaves it there - the strip's own arithmetic run on dragFrom = -1.
    if (state.dragFrom < 0) return;
    const strip = $("strip");
    const dx = stripContentX(state.lastClientX) - state.grabPx;
    // Rounding the held frame's own displacement, not the raw finger, means the
    // gap opens exactly when the frame is SEEN to cross the midpoint.
    state.dragTo = Math.max(
      0,
      Math.min(state.screens.length - 1, Math.round(state.dragFrom + dx / PITCH))
    );
    const cells = strip.querySelectorAll(".cell");
    for (let j = 0; j < cells.length; j++) {
      if (j === state.dragFrom) {
        cells[j].style.transform = "translateX(" + dx + "px) scale(1.05)";
        continue;
      }
      let shift = 0;
      if (state.dragFrom < state.dragTo && j > state.dragFrom && j <= state.dragTo) {
        shift = -PITCH;
      } else if (state.dragFrom > state.dragTo && j >= state.dragTo && j < state.dragFrom) {
        shift = PITCH;
      }
      cells[j].style.transform = shift ? "translateX(" + shift + "px)" : "";
    }
  }

  function stopAutoScroll() {
    if (state.autoRaf) cancelAnimationFrame(state.autoRaf);
    state.autoRaf = 0;
    state.autoLastT = 0;
  }

  // Signed px/s: negative scrolls left, 0 means the finger is clear of both
  // zones. Proportional to depth, so the edge itself is the fastest point.
  function autoScrollSpeed(clientX) {
    const rect = $("strip").getBoundingClientRect();
    const fromLeft = clientX - rect.left;
    const fromRight = rect.right - clientX;
    if (fromLeft < EDGE_ZONE) {
      return -MAX_SPEED * Math.min(1, (EDGE_ZONE - fromLeft) / EDGE_ZONE);
    }
    if (fromRight < EDGE_ZONE) {
      return MAX_SPEED * Math.min(1, (EDGE_ZONE - fromRight) / EDGE_ZONE);
    }
    return 0;
  }

  function autoScrollFrame(t) {
    const strip = $("strip");
    const speed = autoScrollSpeed(state.lastClientX);
    if (state.dragIdx === null || speed === 0) {
      stopAutoScroll();
      return;
    }
    // Delta-timed so a 120Hz panel scrolls at the same speed as a 60Hz one.
    // Clamped because a backgrounded tab can hand back a huge first delta.
    const dt = state.autoLastT ? Math.min(64, t - state.autoLastT) : 16;
    state.autoLastT = t;
    // state.autoScrollMax, not a fresh strip.scrollWidth read - see its
    // comment in state and the capture in onFrameDown: scrollWidth grows
    // along with the held cell's own translateX once that crosses the
    // un-inflated content edge, so it cannot be trusted as a live stopping
    // target.
    const max = state.autoScrollMax;
    const next = Math.max(0, Math.min(max, strip.scrollLeft + (speed * dt) / 1000));
    strip.scrollLeft = next;
    // The finger has not moved, but the content under it has, so the target
    // slot must be recomputed on every frame rather than on pointermove alone.
    applyDragVisuals();
    if ((speed < 0 && next <= 0) || (speed > 0 && next >= max - 0.5)) {
      stopAutoScroll();
      return;
    }
    state.autoRaf = requestAnimationFrame(autoScrollFrame);
  }

  // Starts the loop when the finger enters a zone, stops it when it leaves.
  // Idempotent: safe to call on every pointermove.
  function updateAutoScroll() {
    if (state.dragIdx === null || autoScrollSpeed(state.lastClientX) === 0) {
      stopAutoScroll();
      return;
    }
    if (!state.autoRaf) {
      state.autoLastT = 0;
      // state.autoScrollMax is already set - captured at pickup in
      // onFrameDown's hold-timer callback, before any loop could start.
      state.autoRaf = requestAnimationFrame(autoScrollFrame);
    }
  }

  function onFrameDown(i, e) {
    // A frame is already held - a second finger must not re-seed the gesture
    // and re-render the strip out from under it.
    if (state.dragIdx !== null) return;
    state.dragPointerId = e.pointerId;
    state.lastClientX = e.clientX;
    state.holdStartX = e.clientX;
    state.holdStartY = e.clientY;
    clearTimeout(state.holdTimer);
    // A short tap opens the sheet; ~280ms without much horizontal travel
    // picks the frame up instead.
    state.holdTimer = setTimeout(() => {
      const strip = $("strip");
      state.dragIdx = i;
      state.dragFrom = i;
      state.dragTo = i;
      state.grabPx = stripContentX(state.lastClientX);
      // Measured at pickup, where the held cell's transform is still zero:
      // during the drag its growing translateX extends the strip's scrollable
      // overflow, so scrollWidth read any later is inflated and the right-edge
      // halt would chase a receding target.
      state.autoScrollMax = strip.scrollWidth - strip.clientWidth;
      strip.classList.add("dragging");
      // Capture on the strip, not the cell: the cell can be re-rendered out
      // from under a capture, and the strip never is.
      try {
        strip.setPointerCapture(state.dragPointerId);
      } catch (err) {
        // Pointer already released - the drag simply never starts.
      }
      // NO renderStrip() here, and it is load-bearing: rebuilding would
      // detach the very cell this touch started on, and touch events keep
      // firing at the detached node WITHOUT bubbling - the touchmove guard in
      // wire() would never see another event for this touch, the browser
      // would claim the pan, and the drag would die with pointercancel.
      // (Pointer events retarget to the strip via the capture above; touch
      // events do not follow pointer capture.) The lift is painted onto the
      // EXISTING cells instead; commitDrag()'s rebuild cleans all of it up
      // after the gesture is over.
      const cells = strip.querySelectorAll(".cell");
      if (cells[i]) cells[i].classList.add("drag");
      $("stripHint").textContent = "Drop it where you want it.";
      applyDragVisuals();
    }, 280);
  }

  function onStripMove(e) {
    if (state.dragIdx === null) {
      // No frame is held, so this touch belongs to the browser: browsing is
      // native panning again (with native inertia and snap - see the
      // touchmove guard in wire()). JS's only interest here is cancelling a
      // pending pick-up once the finger has clearly set off to scroll or to
      // drift. Touch mostly self-cancels - the compositor claims the pan and
      // fires pointercancel, which routes through onStripUp - but a mouse is
      // never claimed, and a slow touch can stay under the browser's slop.
      if (
        state.holdTimer &&
        (Math.abs(e.clientX - state.holdStartX) > 8 ||
          Math.abs(e.clientY - state.holdStartY) > 8)
      ) {
        clearTimeout(state.holdTimer);
        state.holdTimer = null;
      }
      return;
    }
    // Pointer capture is per-pointerId, so a second finger touching the strip
    // while one is already held still reaches this handler. Only the finger
    // that picked the frame up (onFrameDown) may move it or steer auto-scroll.
    if (e.pointerId !== state.dragPointerId) return;
    // Note this does NOT hold the strip still - preventDefault on pointermove
    // is not what stops a scroll; touch-action in style.css is. It is kept for
    // the legacy compatibility mouse events some engines still synthesise.
    e.preventDefault();
    // y is ignored on purpose: the frame stays on the horizontal rail however
    // far the finger wanders off the strip.
    state.lastClientX = e.clientX;
    applyDragVisuals();
    updateAutoScroll();
  }

  const DROP_MS = 180; // must match the .strip.dropping transform transition

  // Flies the held frame from wherever the finger left it to its resting slot,
  // then commits. dragIdx stays non-null for the whole flight so the 10s poll's
  // `if (state.dragIdx !== null) return` guard keeps a rebuild out of it.
  function startDrop() {
    stopAutoScroll();
    const strip = $("strip");
    const held = strip.querySelectorAll(".cell")[state.dragFrom];
    if (!held) {
      commitDrag();
      return;
    }
    strip.classList.remove("dragging");
    strip.classList.add("dropping");
    // Flush the class change so the transition swap (none -> 0.18s) is in
    // effect BEFORE the transform below changes; batched into one frame, the
    // browser would see only the end state and skip the animation.
    void strip.offsetWidth;
    held.style.transform = "translateX(" + (state.dragTo - state.dragFrom) * PITCH + "px) scale(1)";
    clearTimeout(state.dropTimer);
    // .dropping is NOT removed here. It has to outlive commitDrag()'s
    // renderStrip() call - see the comment on that removal, in commitDrag,
    // for why the order matters.
    state.dropTimer = setTimeout(commitDrag, DROP_MS);
  }

  // The single exit for a held frame: commit the move, unfreeze the DOM.
  function commitDrag() {
    stopAutoScroll();
    const strip = $("strip");
    moveScreen(state.dragFrom, state.dragTo);
    state.dragIdx = null;
    state.dragFrom = -1;
    state.dragTo = -1;
    state.dragPointerId = null;
    state.grabPx = 0;
    state.lastClientX = 0;
    clearTimeout(state.dropTimer);
    state.dropTimer = 0;
    state.autoScrollMax = 0; // re-measure fresh next gesture, not a stale/inflated value
    strip.classList.remove("dragging");
    // Rebuilding clears every inline transform, and lands the cells in the
    // order the transforms were already showing - so there is nothing to see.
    renderStrip();
    // .dropping is removed only now, AFTER the rebuild above, not before it
    // (not from startDrop's timer before calling this). Keeping it
    // on through renderStrip()'s relayout is what guarantees scroll-snap
    // cannot disturb the strip during the 180ms flight or during the rebuild
    // that immediately follows it - confirmed live: with .strip.dropping in
    // the snap-suppression rule (style.css), scrollLeft is provably frozen
    // for the whole flight regardless of this remove-vs-rebuild order, since
    // both happen inside one synchronous tick and the browser only evaluates
    // snap on its own next paint either way. What the order guards against is
    // an engine that (unlike the one this was built against) reconsiders snap
    // synchronously on the class mutation itself, before the rebuild's own
    // relayout has run - removing it here keeps the class present for that
    // relayout no matter how eagerly a browser applies it.
    // One thing this does NOT prevent, and is not meant to: once the class
    // actually comes off, scrollLeft is free to settle onto the nearest snap
    // point on the very next frame if it isn't on one already. That happens
    // only after the rebuild above has already repainted the dropped frame in
    // its final resting place, so it is a harmless post-hoc realignment, not
    // the "wrong spot" mid-flight teleport this class ordering and the CSS
    // change both guard against.
    strip.classList.remove("dropping");
  }

  // Takes the event (unlike its name-only predecessor) so the drag half below
  // can tell whose pointer this is. wire() binds this directly to the strip's
  // pointerup and pointercancel, which is what makes a pointercancel FOR THE
  // DRAGGING POINTER still end the drag correctly. onStripLeave is the only
  // internal caller and passes no event - see the guard below for why that is
  // safe rather than a hole.
  function onStripUp(e) {
    clearTimeout(state.holdTimer);
    state.holdTimer = null;
    // Browsing is native again, so a swipe's release needs nothing from JS:
    // the browser runs the fling, snaps to a frame, and swallows the click
    // that follows its own scroll. Only a held frame has business here.
    if (state.dragIdx === null) return;
    // A second finger's pointerup/pointercancel must not be able to commit
    // someone else's drag - only the pointer that picked the frame up may
    // drop it. `e` is only ever missing when onStripLeave calls this
    // internally, and it only does so when state.dragIdx is already null
    // (see onStripLeave), so this branch is never reached on that path -
    // there is no real internal caller to accommodate here, but the `e &&`
    // guard is kept so a future no-event caller fails safe instead of
    // throwing on `e.pointerId`.
    if (e && e.pointerId !== state.dragPointerId) return;
    // Suppress the click that follows the release, so the drop does not open
    // the editor for whatever frame ended up under the finger.
    state.justDragged = true;
    setTimeout(() => {
      state.justDragged = false;
    }, 60);
    startDrop();
  }

  // While a frame is held, leaving the strip is not a drop: the frame stays on
  // the rail and the gesture ends on pointerup like any other. Pointer capture
  // means this should not fire mid-drag anyway - this is belt and braces.
  function onStripLeave() {
    if (state.dragIdx !== null) return;
    onStripUp();
  }

  // Where an index lands after the element at `from` is moved to `to`. sel and
  // starIdx are positions, so they have to ride along with a reorder or the
  // star silently jumps to a different screen. -1 (nothing starred) passes
  // straight through.
  function remapIndex(idx, from, to) {
    if (idx < 0) return idx;
    if (idx === from) return to;
    if (from < to && idx > from && idx <= to) return idx - 1;
    if (from > to && idx >= to && idx < from) return idx + 1;
    return idx;
  }

  // A splice-move, not a pairwise swap: the transform-driven drag works out
  // one final destination and commits it once, rather than swapping its way
  // there. N adjacent swaps and one move produce the same array.
  function moveScreen(from, to) {
    if (from === to) return;
    if (from < 0 || to < 0) return;
    if (from >= state.screens.length || to >= state.screens.length) return;
    const moved = state.screens.splice(from, 1)[0];
    state.screens.splice(to, 0, moved);
    state.sel = remapIndex(state.sel, from, to);
    state.starIdx = remapIndex(state.starIdx, from, to);
  }

  function setStar(i) {
    // A radio, not a checkbox: exactly one screen drives the face, and tapping
    // the starred one does nothing. Un-starring would leave screen_id 0 ->
    // MoodBand::Unknown -> a face that never reacts to anything, which reads as
    // a broken device rather than a choice. The star only ever moves.
    if (state.starIdx === i) return;
    state.starIdx = i;
    renderStrip();
    if (state.sheetOpen) bindSheet();
  }

  function addScreen() {
    // The add tile stays visible and clickable at the cap on purpose: a tile
    // that vanishes, or a button that does nothing, both read as a bug. This
    // says what the limit is instead.
    if (state.screens.length >= MAX_SCREENS) {
      flash(
        "screensMsg",
        "Figo holds " + MAX_SCREENS + " screens. Remove one to add another.",
        true
      );
      return;
    }
    state.screens.push({
      srvId: null,
      type: "STOCK",
      symbol: DEFAULT_SYMBOL,
      name: DEFAULT_NAME,
      interval: INTERVAL_DEFAULT,
      layout: "SINGLE",
      symbol2: "",
      name2: "",
    });
    state.sel = state.screens.length - 1;
    renderStrip();
    centerFrame(state.sel);
    openSheet();
    // Price it straight away, so the sheet opens showing a screen rather than a
    // form to fill in.
    checkLeg(state.screens[state.sel], 1);
  }

  // -------------------------------------------------------- editor sheet --

  function openSheet() {
    state.sheetOpen = true;
    const sc = screenAt(state.sel);
    // #findResults is one element shared by every screen; a screen reopened
    // in the find view must start from an empty list, not the last screen's.
    if (sc && sc.ui === "find") {
      clearTimeout(findTimer);
      findSeq++;
      $("findResults").textContent = "";
      $("fFind").value = "";
      paintFindStatus(null, "");
    }
    bindSheet();
    $("sheet").classList.add("open");
    $("backdrop").classList.add("open");
  }

  function closeSheet() {
    // Same reasoning as setMode(): a debounced runFind() armed by typing may
    // still be pending, and this is the one exit from the find view where
    // it could be left running - closing the sheet mid-typing would fire a
    // pointless POST, bumped findSeq, and painted into elements behind a
    // closed sheet ~350ms later.
    clearTimeout(findTimer);
    const sc = screenAt(state.sel);
    // A screen closed from inside the find view would reopen there, showing
    // whatever list the last search left behind - possibly another screen's.
    // Only a screen with nothing to show has a reason to reopen in the search.
    if (sc && sc.ui === "find" && (sc.symbol || sc.type === "FEAR_GREED")) sc.ui = "edit";
    state.sheetOpen = false;
    $("sheet").classList.remove("open");
    $("backdrop").classList.remove("open");
    // Closing is also a return to the edit view: ask again for any leg whose
    // check was cut off or could not reach the device.
    if (sc) recheckInterrupted(sc);
  }

  function bindSheet() {
    const sc = screenAt(state.sel);
    if (!sc) {
      closeSheet();
      return;
    }
    $("sheetTitle").textContent = "Screen " + (state.sel + 1);
    $("sheetAge").textContent = health(sc).text;

    const isFg = sc.type === "FEAR_GREED";
    const dual = isDual(sc);
    // Two states: "find" is the search, "edit" is the screen itself. Client-only
    // and never saved.
    const finding = sc.ui === "find";
    $("sheet").classList.toggle("finding", finding);
    $("findWrap").hidden = !finding;
    $("previewBlock").hidden = finding;
    $("typeBlock").hidden = finding;

    // A gauge names itself after the index it shows, so the name field has
    // nothing to ask and the name is kept in step here.
    if (isFg) sc.name = fgName(sc);

    const nameInput = $("fName");
    nameInput.maxLength = nameMaxFor(sc);
    nameInput.value = sc.name;
    $("fName2").value = sc.name2;
    $("fName2").maxLength = NAME_MAX.DUAL;
    $("nameRow").hidden = isFg;
    $("name2Wrap").hidden = !dual;
    $("nameWrap").querySelector(".field-label").textContent = dual ? "Left name" : "Name on screen";
    $("nameRow").classList.toggle("two", dual);

    renderTypePills(sc);
    renderFgPills(sc);
    renderPills(sc);
    if (finding) paintFindHeader(sc);
    else paintPreview(sc);

    // The thresholds live in Settings now; the sheet only says that this is
    // the screen they apply to.
    $("moodNote").hidden = state.starIdx !== state.sel;
  }

  function renderPills(sc) {
    const choices = isDual(sc) ? INTERVAL_CHOICES_DUAL : INTERVAL_CHOICES;
    $("fIntervalVal").textContent = intervalLabel(sc.interval);
    const box = $("intervalPills");
    box.textContent = "";
    choices.forEach((n) => {
      // A stored interval that is not one of the four (any value >= the floor
      // is legal on the device) lights NO pill and simply shows in the
      // readout above. The first tap replaces it - it is never silently
      // rounded to the nearest choice behind the user's back.
      const b = el("button", sc.interval === n ? "on" : "", intervalLabel(n));
      b.type = "button";
      b.addEventListener("click", () => {
        sc.interval = n;
        renderPills(sc);
        renderStrip();
      });
      box.appendChild(b);
    });
  }

  // ---------------------------------------------------- the screen editor --

  const SOURCE_LABEL = { CRYPTO: "Crypto", STOCK: "Stock", COMMODITY: "Commodity", FEAR_GREED: "Fear & Greed" };
  // The panel's own titles for the two gauges, within the 12-character name.
  const fgName = (sc) => (sc.symbol === "stocks" ? "Fear & Greed" : "Crypto F&G");

  function setMode(mode) {
    const sc = screenAt(state.sel);
    if (!sc) return;
    // A debounced runFind() armed by typing may still be pending; leaving the
    // find view makes it pointless and, worse, lets it fire ~350ms later into
    // whatever mode came next.
    clearTimeout(findTimer);
    sc.ui = mode;
    bindSheet();
  }

  function goFind(leg) {
    const sc = screenAt(state.sel);
    if (!sc) return;
    clearTimeout(findTimer);
    sc.findLeg = leg;
    sc.ui = "find";
    // renderFindResults() filters by provider at RENDER time only - nothing
    // clears #findResults when the find view is entered, so a list rendered
    // for the OTHER leg (or before this leg's provider changed) would stay
    // fully clickable: search "bitcoin" on leg 1 (unfiltered, Yahoo rows
    // included), back out, open leg 2, and those stale Yahoo rows would
    // still be sitting there to tap. Bump findSeq too, so an in-flight
    // search/check left over from the leg just exited can't paint into this
    // fresh view once its poll resolves (awaitJob/runFind both bail as soon
    // as their own `my` stops matching).
    findSeq++;
    // ...and that same bump is what cuts off a price check still polling for
    // a leg already picked: the device's one job slot is about to belong to
    // the search. Say so NOW rather than when the check's next poll notices,
    // so leaving the find view straight away still finds the leg marked, and
    // recheckInterrupted() has something to act on.
    [1, 2].forEach((l) => {
      if (sc[LEG[l].busy]) sc[LEG[l].flag] = "interrupted";
    });
    $("findResults").textContent = "";
    $("fFind").value = "";
    paintFindStatus(null, "");
    bindSheet();
    $("fFind").focus();
  }

  function paintFindHeader(sc) {
    const leg = sc.findLeg === 2 ? 2 : 1;
    $("findLabel").textContent = leg === 2 ? "Find the second instrument" : "Find an instrument";
    // Nothing to go back TO until this screen shows something.
    $("btnFindBack").hidden = !(sc.symbol || sc.type === "FEAR_GREED");
    const note = $("findPairNote");
    note.hidden = leg !== 2;
    if (leg === 2) {
      note.textContent =
        sc.type === "CRYPTO"
          ? "Both halves of a pair come from one source. This screen reads CoinGecko, so only coins fit here."
          : "Both halves of a pair come from one source. This screen reads Yahoo, so a coin has to be its Yahoo pair, like BTC-USD.";
    }
  }

  // --------------------------------------------------- instrument search --
  // The phone cannot call Yahoo's search itself (it sends no CORS headers), so
  // it asks Figo. The device queues the work behind any price fetch in flight -
  // one TLS session at a time - so this POSTs the query and then POLLS for the
  // answer rather than holding one long request open: an AsyncWebServer handler
  // that blocks past ~5s panics the board.
  const FIND_POLL_MS = 400;
  const FIND_GROUPS = [["best", "Best match"], ["yahoo", "Yahoo Finance"], ["coingecko", "Crypto · CoinGecko"]];
  let findSeq = 0;
  let findTimer = null;
  // The per-leg fields a price check reads and writes. `busy` is true exactly
  // while checkLeg() for that leg is in flight, so a caller can tell "a check
  // is out" from "no check was ever asked for".
  const LEG = {
    1: { sym: "symbol", px: "px1", flag: "check1", gen: "_chk1Gen", busy: "_chk1Busy" },
    2: { sym: "symbol2", px: "px2", flag: "check2", gen: "_chk2Gen", busy: "_chk2Busy" },
  };

  function findStep(label, st) {
    const s = el("span", "f-step " + st);
    s.appendChild(el("i"));
    s.appendChild(document.createTextNode(label));
    return s;
  }

  function paintFindStatus(phase, text) {
    const box = $("findStatus");
    box.textContent = "";
    if (phase) {
      const at = ["yahoo", "coingecko", "done"].indexOf(phase);
      box.appendChild(findStep("Yahoo", at > 0 ? "ok" : "run"));
      box.appendChild(findStep("CoinGecko", at > 1 ? "ok" : at === 1 ? "run" : "wait"));
    }
    if (text) box.appendChild(document.createTextNode(text));
  }

  // Polls /api/search/result until the job matching `seq` finishes. Returns
  // the finished job; null when this call has been superseded by a newer one
  // (silent - that UI is already gone); or the string "timeout" when the
  // 40-poll bound is exhausted without the job ever reaching a terminal
  // state. The device sets no TTL on its one job slot by design, so this
  // bound is the only timeout anywhere in the system, and the two must stay
  // distinguishable: a caller that treats a timeout as a silent supersession
  // leaves its own UI stuck (spinning chips, a "fetching..." caption) with
  // nothing actually running.
  //
  // One failed poll is not a failed job: the device can answer a single GET
  // with a 503 (its state mutex was busy) or drop a connection, and the job it
  // is running is unaffected. So a poll error is retried, still inside the
  // 40-poll bound; only three in a row - the device really is unreachable -
  // is thrown to the caller.
  async function awaitJob(seq, my, onState) {
    let pollErrors = 0;
    for (let i = 0; i < 40; i++) {
      await new Promise((r) => setTimeout(r, FIND_POLL_MS));
      if (my !== findSeq) return null;
      let res;
      try {
        res = await apiGet("/api/search/result");
        pollErrors = 0;
      } catch (e) {
        if (++pollErrors >= 3) throw e;
        continue;
      }
      // A job the device has replaced is one this phone is no longer waiting on.
      if (res.seq > seq) return null;
      if (res.seq < seq) continue;
      if (res.state === "done" || res.state === "failed") return res;
      if (onState) onState(res.state);
    }
    return "timeout";
  }

  async function runFind(q) {
    q = q.trim();
    $("findResults").textContent = "";
    if (q.length < 2) {
      paintFindStatus(null, "");
      return;
    }
    // Bumped only once a search is actually going to happen - bumping it for
    // every keystroke (including one that backspaces below the 2-char floor)
    // would cancel an in-flight checkLeg() too, since it shares this counter.
    const my = ++findSeq;
    paintFindStatus("yahoo", "");
    let res;
    try {
      const queued = await apiPost("/api/search", { q });
      res = await awaitJob(queued.seq, my, (st) => paintFindStatus(st, ""));
    } catch (e) {
      if (my === findSeq) {
        paintFindStatus(null, "Figo couldn't search just now. Check it is online and try again.");
      }
      return;
    }
    // Rendering sits OUTSIDE the try: a render-time exception must not be
    // reported to the user as a network failure.
    if (my !== findSeq || res === null) return; // superseded; that UI is already gone
    if (res === "timeout") {
      paintFindStatus(null, "Figo couldn't search just now. Check it is online and try again.");
      return;
    }
    renderFindResults(res, q);
  }

  // Hand-checked instruments that rank ABOVE the provider's own order. Yahoo is
  // already right for company names (figma -> FIG, klarna -> KLAR), and wrong
  // for concepts: "shekel" returns bond indices, "gold" returns Gold.com Inc.,
  // "s&p 500" returns the SPY fund above the index. This list is that
  // correction, not a catalogue - anything absent still appears from Yahoo.
  // [source, symbol, name, detail, why]
  const PICKS = {
    "shekel": [["STOCK", "ILS=X", "US dollar → Israeli shekel", "Currency",
                "How many ₪ one dollar buys. Yahoo's own top hits for “shekel” are bond indices."],
               ["STOCK", "EURILS=X", "Euro → Israeli shekel", "Currency", ""]],
    "gold": [["COMMODITY", "GC=F", "Gold", "Futures · COMEX",
              "USD per troy ounce. Yahoo ranks the company Gold.com first."]],
    "oil": [["COMMODITY", "CL=F", "Crude oil (WTI)", "Futures · NYMEX", "USD per barrel."]],
    "s&p 500": [["STOCK", "^GSPC", "S&P 500", "Index", "The index itself. Yahoo ranks the SPY fund above it."]],
    "bitcoin": [["CRYPTO", "bitcoin", "Bitcoin", "Crypto · CoinGecko #1", ""]],
    "ta35": [["STOCK", "TA35.TA", "TA-35", "Index · Tel Aviv",
              "The Tel Aviv Stock Exchange's flagship index. Yahoo's own results for “ta35” don't lead with it."]],
  };
  const ALIAS = {
    "s&p": "s&p 500", "sp500": "s&p 500", "spx": "s&p 500", "s&p500": "s&p 500",
    "btc": "bitcoin", "ils": "shekel", "nis": "shekel", "usdils": "shekel",
    "crude": "oil", "wti": "oil", "xau": "gold", "ta-35": "ta35", "tase": "ta35",
  };
  // Names the panel fits, for symbols whose provider name does not shorten well.
  const NICK = {
    "TEVA": "Teva", "TEVA.TA": "Teva", "LUMI.TA": "Leumi", "GC=F": "Gold", "CL=F": "Oil WTI",
    "BZ=F": "Brent", "^GSPC": "S&P 500", "AAPL": "Apple", "bitcoin": "Bitcoin",
    "BTC-USD": "Bitcoin", "ILS=X": "USD/ILS", "EURILS=X": "EUR/ILS", "TA35.TA": "TA-35",
  };

  // The shortest honest name within the panel's budget.
  function nickFor(symbol, name, max) {
    if (NICK[symbol]) return NICK[symbol].slice(0, max);
    let n = name.replace(/,.*$/, "").replace(/\(.*?\)/g, "")
      .replace(/\b(Inc\.?|Ltd\.?|Limited|Corporation|Corp\.?|plc|B\.M\.|LP|Trust|Group|Industries|The|USD)\b/gi, "")
      .replace(/\s+/g, " ").trim();
    // The strip pass can leave a dangling separator behind ("USD/TRY" ->
    // "/TRY") or, when the whole name was made of stripped words, nothing at
    // all - trim those edges and fall back to the symbol rather than a
    // leading slash or an empty ticker name. The result has to be ASCII: the
    // firmware counts BYTES against the name limit, and the compiled Montserrat
    // fonts hold no glyph outside 0x20-0x7E, so an accent would both overflow
    // the limit and draw as a blank box. Accents are decomposed and dropped
    // first ("Nestlé" -> "Nestle") rather than chopping the letter; a wholly
    // non-Latin name ends up empty and falls back to its bare symbol.
    n = n.normalize("NFD").replace(/\p{M}+/gu, "");
    n = n.replace(/[^\x20-\x7e]/g, "").replace(/\s+/g, " ");
    n = n.replace(/^[^A-Za-z0-9]+/, "").replace(/[^A-Za-z0-9]+$/, "");
    if (!n) return symbol.slice(0, max);
    if (n.length <= max) return n;
    let out = "";
    for (const w of n.split(" ")) {
      if ((out + " " + w).trim().length > max) break;
      out = (out + " " + w).trim();
    }
    return out || n.slice(0, max);
  }

  function picksFor(q) {
    const key = PICKS[q] ? q : ALIAS[q];
    return (PICKS[key] || []).map(([source, symbol, name, detail, why]) =>
      ({ group: "best", source, symbol, name, detail, why }));
  }

  // A pair is fetched by ONE source: fetchScreen() sends both symbols to
  // CoinGecko for a CRYPTO screen and to Yahoo otherwise. So the second half
  // can only offer what this screen's source can fetch. These are the coins
  // Yahoo prices too, which is what makes a coin + stock pair possible at all.
  const CG_TO_YAHOO = {
    bitcoin: "BTC-USD", ethereum: "ETH-USD", solana: "SOL-USD", ripple: "XRP-USD",
    dogecoin: "DOGE-USD", cardano: "ADA-USD", litecoin: "LTC-USD", "bitcoin-cash": "BCH-USD",
    tron: "TRX-USD", monero: "XMR-USD", stellar: "XLM-USD", polkadot: "DOT-USD",
  };
  const familyOf = (source) => (source === "CRYPTO" ? "CG" : "YAHOO");

  // The only two routes that can change a pair's provider
  // (switchPairToYahoo() and a leg-1 pick that changes sc.type) must not
  // leave leg 2 exactly as it was, since the OTHER leg's provider just
  // changed underneath it. setScreenKind() already has a "superseded"
  // pattern for a leg whose instrument no longer applies - but here we go
  // further and clear symbol2/name2 outright rather than merely flagging
  // check2. Reasoning: the tap-zone's "+ Add" empty state is a far louder,
  // clearer signal to re-pick than a caption that quietly goes blank
  // (checkLine() returns ["",""] for a dual screen with no sc.symbol2 at
  // all) - a name still sitting there ("Ethereum") while nothing ever prices
  // it again reads as "this is still set", not "this needs attention".
  // check2 = "superseded" (not merely clearing px2) is still required even
  // with symbol2 blanked: previewValue()'s fromSaved2 falls back to
  // state.values[srvId].price2 - the DEVICE's last poll for the OLD pair -
  // whenever check2 is falsy, and that saved number still describes the
  // instrument this screen held before the edit.
  function invalidateLeg2(sc) {
    sc.symbol2 = "";
    sc.name2 = "";
    sc.px2 = null;
    sc.check2 = "superseded";
  }

  // Says WHY rows were withheld, and offers the way out when there is one.
  function pairNote(sc, dropped, family) {
    const wrap = el("div", "f-pairnote");
    if (family === "YAHOO") {
      wrap.appendChild(el("p", "f-note", dropped + " CoinGecko " + (dropped === 1 ? "coin" : "coins") +
        " hidden: this screen reads Yahoo. A coin listed above as X-USD is that same coin, priced by Yahoo."));
      return wrap;
    }
    wrap.appendChild(el("p", "f-note", dropped + " Yahoo " + (dropped === 1 ? "result" : "results") +
      " hidden: this screen reads CoinGecko, which prices coins only."));
    const twin = twinFor(sc);
    if (twin) {
      const b = el("button", "f-swap",
        "Move this screen to Yahoo (" + (sc.name || sc.symbol) + " → " + twin + ")");
      b.type = "button";
      b.addEventListener("click", switchPairToYahoo);
      wrap.appendChild(b);
    }
    return wrap;
  }

  const twinFor = (sc) => CG_TO_YAHOO[String(sc.symbol).toLowerCase()];

  // The twin is read from the screen when the button is TAPPED, not captured
  // when the button was drawn: the list it sits in can outlive the screen it
  // was drawn for (closing the sheet from the find view and opening another
  // screen), and a captured twin would then convert THIS screen to that one's
  // coin.
  async function switchPairToYahoo() {
    const sc = screenAt(state.sel);
    const twin = sc && twinFor(sc);
    if (!twin) return;
    sc.type = "STOCK";
    sc.symbol = twin;
    sc.px1 = null;
    sc.check1 = "running";
    // This rewrites leg 1 (type/symbol/px1) - leg 2 cannot just be left
    // alone. A pair being edited already has its OTHER half
    // filled from the provider being left - e.g. editing a bitcoin+ethereum
    // pair's second half, symbol2 already holds "ethereum" with check2 "ok"
    // and a CoinGecko px2. Once this leg re-checks ok, previewValue() has
    // both legs and checkLine() reports "Figo priced both." - a confident
    // price under a half the device can never fetch (fetchScreen() sends
    // both legs to ONE provider). See invalidateLeg2()'s own comment for why
    // it clears symbol2/name2 outright rather than merely flagging check2.
    invalidateLeg2(sc);
    renderStrip();
    // The note above the results box still names the OLD provider until
    // this repaints it - switchPairToYahoo() is one of only two places
    // sc.type changes (the other, pickResult(), goes through bindSheet()'s
    // full repaint via sc.ui="edit"). bindSheet() itself is wrong here: the
    // user may still be typing in #fFind, and bindSheet() would fight that.
    // paintFindHeader() alone repaints just the header.
    paintFindHeader(sc);
    // Both of these bump the shared findSeq counter (see checkLeg()'s NOTE),
    // so they must run one at a time or the second cancels the first through
    // that counter. runFind() goes first: it is what makes the list actually
    // reflect the new provider - the whole point of this button - so it must
    // finish and paint before checkLeg() takes the counter for leg 1's
    // re-check.
    await runFind($("fFind").value);
    await checkLeg(sc, 1);
  }

  function renderFindResults(res, q) {
    const sc = screenAt(state.sel);
    const device = (res.results || []).map((r) =>
      Object.assign({ group: r.source === "CRYPTO" ? "coingecko" : "yahoo" }, r));
    const picks = picksFor(q.toLowerCase());
    const taken = new Set(picks.map((p) => p.symbol));
    const rows = picks.concat(device.filter((r) => !taken.has(r.symbol)));
    // The second half can only offer this screen's own provider - filtered here
    // rather than refused at save time.
    const leg = sc && sc.findLeg === 2 ? 2 : 1;
    const family = sc ? familyOf(sc.type) : "YAHOO";
    const shown = leg === 2 ? rows.filter((r) => familyOf(r.source) === family) : rows;
    const dropped = rows.length - shown.length;

    const box = $("findResults");
    box.textContent = "";
    if (!rows.length) {
      // Genuinely nothing - the provider filter never even entered into it.
      paintFindStatus("done", "");
      box.appendChild(el("p", "f-empty",
        "Nothing found for “" + q + "”. Try the company's name, or a shorter word."));
      return;
    }
    if (!shown.length) {
      // Rows existed but the provider filter withheld every one of
      // them - this is NOT "nothing found" (that phrasing is false, and it
      // hides the fix: pairNote()'s swap button, which exists for exactly
      // this case - a leg-2 search for a stock on a CoinGecko screen, or a
      // coin on a Yahoo screen, must still offer the way out).
      paintFindStatus("done", "");
      box.appendChild(el("p", "f-empty",
        "Found " + rows.length + " for “" + q + "”, but none fit this screen's source."));
      box.appendChild(pairNote(sc, dropped, family));
      return;
    }
    // The honest count: what's actually clickable below, not the pre-filter
    // total the provider filter may have shrunk - otherwise "12 results"
    // can sit above a 2-row list.
    paintFindStatus("done", shown.length + " results");
    FIND_GROUPS.forEach(([key, title]) => {
      const items = shown.filter((r) => r.group === key);
      if (!items.length) return;
      const g = el("div", "f-group " + key);
      g.appendChild(el("div", "f-head", title));
      items.forEach((r) => {
        const b = el("button", "f-row");
        b.type = "button";
        b.appendChild(el("span", "f-name", r.name));
        b.appendChild(el("span", "f-sym", r.symbol));
        b.appendChild(el("span", "f-meta", r.detail));
        b.appendChild(el("span", "f-src", SOURCE_LABEL[r.source]));
        if (r.why) b.appendChild(el("span", "f-why", r.why));
        b.addEventListener("click", () => pickResult(r));
        g.appendChild(b);
      });
      box.appendChild(g);
    });
    if (dropped > 0) box.appendChild(pairNote(sc, dropped, family));
    if (res.hidden_coins) {
      box.appendChild(el("p", "f-note", res.hidden_coins + " CoinGecko " +
        (res.hidden_coins === 1 ? "coin" : "coins") +
        " hidden: tokenized stocks and coins outside the top 500."));
    }
  }

  // One price fetch for one symbol - the same request this screen will make on
  // its schedule - so a symbol that cannot be priced says so HERE rather than
  // becoming a blank frame on the desk an interval later.
  //
  // The leg's flag ends as one of:
  //   "ok" / "failed"  the device answered: priced, or it could not price it.
  //   "interrupted"    the answer was never collected - a search took the
  //                    device's one job slot first. The symbol is fine; nothing
  //                    is known about it yet. recheckInterrupted() asks again.
  //   "error"          the device could not be reached. Also unknown, also
  //                    retried by recheckInterrupted(), and worded differently
  //                    from "failed" because it says nothing about the symbol.
  async function checkLeg(sc, leg) {
    const L = LEG[leg];
    const symbol = sc[L.sym];
    // Leg checks share findSeq with the search box's own polling. Two
    // checkLeg() calls issued together (a dual instrument's two legs) would
    // cancel each other through that shared counter - so leg checks run one
    // at a time (recheckInterrupted() is the only place that runs two).
    const my = ++findSeq;
    // findSeq also moves when the user taps into a search, which says nothing
    // about whether THIS leg's symbol is still valid. L.gen is a second,
    // per-leg ticket, independent of findSeq: it only advances when
    // checkLeg() is called again for this SAME leg (a genuine re-pick or a
    // re-check), so it is the guard for "has this leg actually been
    // superseded by a newer check" - not "did the shared counter move".
    const myGen = (sc[L.gen] || 0) + 1;
    sc[L.gen] = myGen;
    sc[L.busy] = true;
    try {
      const queued = await apiPost("/api/search/check", { type: sc.type, symbol: symbol });
      const res = await awaitJob(queued.seq, my, null);
      // The user moved on to another instrument while this was out.
      if (sc[L.sym] !== symbol) return;
      // A newer checkLeg() for this SAME leg has already taken over - it owns
      // the outcome now; stamping anything here would race it.
      if (sc[L.gen] !== myGen) return;
      if (res === "timeout") {
        // The 40-poll bound ran out with the device never reporting a
        // terminal state for this job. "failed" is honest: we stopped waiting
        // for an answer that is not coming.
        sc[L.px] = null;
        sc[L.flag] = "failed";
      } else if (res === null) {
        // Cut off: a search or another leg's check took findSeq before this
        // job resolved. Leaving the flag at "running" would lie (nothing is
        // in flight any more) and "failed" would lie the other way
        // (checkLine() reads it as the provider rejecting the symbol).
        // Any truthy flag also keeps previewValue() from resurrecting the
        // OLD instrument's saved price under this leg.
        sc[L.flag] = "interrupted";
      } else {
        sc[L.px] = res.check_ok
          ? { price: res.price, change_pct: res.change_pct, currency: res.currency }
          : null;
        sc[L.flag] = res.check_ok ? "ok" : "failed";
      }
    } catch (e) {
      // Unreachable, not rejected - unless a newer check has taken over.
      if (sc[L.sym] === symbol && sc[L.gen] === myGen) sc[L.flag] = "error";
    } finally {
      if (sc[L.gen] === myGen) sc[L.busy] = false;
    }
    if (state.sheetOpen && screenAt(state.sel) === sc) bindSheet();
    refreshStages();
  }

  // Re-asks for every leg whose check was cut off or could not reach the
  // device. Called wherever the sheet is back in its edit view (a pick, Back,
  // closing the sheet): no search can be in flight then, so this cannot take
  // the user's job slot.
  // Legs go one at a time (each check bumps findSeq and would cancel the
  // other), and `_rechecking` keeps two callers from interleaving.
  // `retryErrors` is off right after a pick: a leg that could not reach the
  // device a moment ago will not fare better now, and retrying at once would
  // hide the "couldn't reach the device" message behind a second failure.
  async function recheckInterrupted(sc, retryErrors = true) {
    if (!sc || sc._rechecking) return;
    sc._rechecking = true;
    try {
      for (const leg of [1, 2]) {
        // A search owns the job slot again, or there is nothing to price.
        if (sc.ui === "find" || sc.type === "FEAR_GREED") return;
        const L = LEG[leg];
        if (!sc[L.sym] || (leg === 2 && !isDual(sc))) continue;
        if (sc[L.flag] !== "interrupted" && !(retryErrors && sc[L.flag] === "error")) continue;
        sc[L.flag] = "running";
        if (state.sheetOpen && screenAt(state.sel) === sc) bindSheet();
        await checkLeg(sc, leg);
      }
    } finally {
      sc._rechecking = false;
    }
  }

  async function pickResult(r) {
    const sc = screenAt(state.sel);
    if (!sc) return;
    clearTimeout(findTimer);
    // Which half this pick fills. Leg 2 only ever touches the *2 fields;
    // leg 1 keeps the full instrument-change path (type/layout/name/px1).
    const leg = sc.findLeg === 2 ? 2 : 1;
    if (leg === 2) {
      // renderFindResults() already filters by provider at render time, and
      // goFind() clears the list on entry so a stale, wrongly-filtered
      // render from a PRIOR view can't sit there clickable - but this is
      // where the value actually FLOWS into sc.symbol2, so it is re-checked
      // here too, independent of whatever the DOM happened to show. A silent
      // no-op would be its own bug (a tap that does nothing, with no
      // explanation) - refuse with a status line instead, the same element
      // runFind()'s own errors use.
      if (familyOf(r.source) !== familyOf(sc.type)) {
        paintFindStatus(null, "That result doesn't fit this screen's source. Search again.");
        return;
      }
      sc.symbol2 = r.symbol;
      // The name comes from the instrument, trimmed to what the panel fits. A
      // name the user then types is theirs until they change the instrument.
      sc.name2 = nickFor(r.symbol, r.name, NAME_MAX.DUAL);
      sc.px2 = null;
      sc.check2 = "running";
    } else {
      // pickResult() and switchPairToYahoo() are the only two routes that
      // can change a pair's provider (setScreenKind() never flips
      // CRYPTO<->STOCK/COMMODITY) - so this leg-1 branch must not change
      // sc.type under an already-filled symbol2 without a glance at it.
      // Picking AAPL as the left half of a bitcoin+ethereum pair must not
      // leave type:"STOCK" with symbol2 still "ethereum", check2 "ok".
      // Compare families, not sc.type === r.source directly: COMMODITY and
      // STOCK are different types but the SAME provider, so a leg-1 pick
      // that stays within Yahoo must not disturb a Yahoo-paired leg 2.
      const familyChanged = sc.symbol2 && familyOf(sc.type) !== familyOf(r.source);
      sc.type = r.source;
      const opts = LAYOUTS_BY_TYPE[sc.type] || LAYOUTS_BY_TYPE.CRYPTO;
      if (!opts.some(([v]) => v === sc.layout)) sc.layout = opts[0][0];
      sc.symbol = r.symbol;
      sc.name = nickFor(r.symbol, r.name, nameMaxFor(sc));
      sc.px1 = null;
      sc.check1 = "running";
      // Same invalidation switchPairToYahoo() uses on the other route that
      // can change the provider - the two must agree, or a reader has to
      // learn two rules for what is really one constraint.
      if (familyChanged) invalidateLeg2(sc);
    }
    sc.ui = "edit";
    applyLayoutRules(sc);
    bindSheet();
    renderStrip();
    await checkLeg(sc, leg);
    // Tapping into this search cut off the OTHER leg's check if it was still
    // out (goFind() bumps findSeq). The sheet is back in its edit view, so no
    // search can be in flight: ask that leg again.
    await recheckInterrupted(sc, false);
  }

  function unitNote(cur, price) {
    const c = String(cur || "").toUpperCase();
    if (c === "ILA") {
      return " Tel Aviv quotes in agorot: " + formatPrice(price, cur) + " means ₪" + (price / 100).toFixed(2) + ".";
    }
    if (c && c !== "USD" && c !== "ILS") return " Priced in " + c + ", as its exchange quotes it.";
    return "";
  }

  function checkLine(sc) {
    if (sc.type === "FEAR_GREED" || !sc.symbol) return ["", ""];
    const dual = isDual(sc);
    if (dual && !sc.symbol2) return ["", ""];
    const legs = dual ? [1, 2] : [1];
    const flags = legs.map((l) => (l === 2 ? sc.check2 : sc.check1));
    const syms = legs.map((l) => (l === 2 ? sc.symbol2 : sc.symbol));
    const running = flags.indexOf("running");
    if (running >= 0) return ["", "Figo is fetching " + syms[running] + " once to check it…"];
    const unreachable = flags.indexOf("error");
    if (unreachable >= 0) {
      return ["failed", "Figo couldn't reach the device to check " + syms[unreachable] +
        ". Close this screen and open it again to retry."];
    }
    const bad = flags.indexOf("failed");
    if (bad >= 0) return ["failed", "Figo couldn't price " + syms[bad] + ". Tap it and pick another."];
    if (flags.every((f) => f === "ok")) {
      let t = "✓ Figo priced " + (dual ? "both." : "it.");
      if (sc.px1) t += unitNote(sc.px1.currency, sc.px1.price);
      if (dual && sc.px2) t += unitNote(sc.px2.currency, sc.px2.price);
      return ["ok", t];
    }
    return ["", ""];
  }

  // The frame is the control: a tap on a half opens the search for that half.
  function tapZone(sc, leg, where) {
    const symbol = leg === 2 ? sc.symbol2 : sc.symbol;
    const b = el("button", "tap-zone " + where + (symbol ? "" : " empty"), symbol ? "" : "+ Add");
    b.type = "button";
    const label = symbol ? "Change " + symbol : "Add an instrument";
    b.title = label;
    b.setAttribute("aria-label", label);
    b.addEventListener("click", () => goFind(leg));
    return b;
  }

  function paintPreview(sc) {
    const isFg = sc.type === "FEAR_GREED";
    const dual = isDual(sc);

    const slot = $("pickedFrame");
    slot.textContent = "";
    const frame = el("div", "frame");
    const stage = buildStage(sc);
    frame.appendChild(stage);
    slot.appendChild(frame);
    // After insertion: fitStagePrices() needs real layout to measure.
    fitStagePrices(stage);

    if (!isFg) {
      if (dual) {
        frame.appendChild(tapZone(sc, 1, "left"));
        frame.appendChild(tapZone(sc, 2, "right"));
      } else {
        frame.appendChild(tapZone(sc, 1, "all"));
      }
    }
    $("pickedHint").textContent = isFg
      ? "The gauge follows the index below."
      : dual
        ? "Tap a half to change what it shows."
        : "Tap the screen to change what it shows.";

    const [cls, text] = checkLine(sc);
    $("pickedCheck").className = "picked-check " + cls;
    $("pickedCheck").textContent = text;
  }

  // One row for "what kind of screen is this?". Single and Dual are layouts of
  // a ticker and Fear & Greed is a different source entirely, but from the
  // user's side all three are the same choice.
  const TYPE_PILLS = [["SINGLE", "Single"], ["DUAL", "Dual"], ["FG", "Fear & Greed"]];
  const FG_INDEX = [["crypto", "Crypto"], ["stocks", "Stock market"]];
  const FG_STYLE = [["SLIDER", "Slider"], ["CIRCULAR", "Circular"]];

  function typeKey(sc) {
    if (sc.type === "FEAR_GREED") return "FG";
    return sc.layout === "DUAL" ? "DUAL" : "SINGLE";
  }

  function pillRow(boxId, valId, choices, cur, onPick) {
    const label = (choices.find(([v]) => v === cur) || [])[1] || "";
    if (valId) $(valId).textContent = label;
    const box = $(boxId);
    box.textContent = "";
    choices.forEach(([v, text]) => {
      const b = el("button", v === cur ? "on" : "", text);
      b.type = "button";
      b.addEventListener("click", () => onPick(v));
      box.appendChild(b);
    });
  }

  function renderTypePills(sc) {
    pillRow("typePills", "fTypeVal", TYPE_PILLS, typeKey(sc), setScreenKind);
  }

  function renderFgPills(sc) {
    const isFg = sc.type === "FEAR_GREED";
    $("fgIndexBlock").hidden = !isFg || sc.ui === "find";
    $("fgStyleBlock").hidden = !isFg || sc.ui === "find";
    if (!isFg) return;
    pillRow("fgIndexPills", "fIndexVal", FG_INDEX, sc.symbol === "stocks" ? "stocks" : "crypto", (v) => {
      sc.symbol = v;
      sc.name = fgName(sc);
      bindSheet();
      renderStrip();
    });
    pillRow("fgStylePills", "fStyleVal", FG_STYLE, sc.layout === "CIRCULAR" ? "CIRCULAR" : "SLIDER", (v) => {
      sc.layout = v;
      bindSheet();
      renderStrip();
    });
  }

  function setScreenKind(kind) {
    const sc = screenAt(state.sel);
    if (!sc || typeKey(sc) === kind) return;
    if (kind === "FG") {
      // Remember what it was, so coming back doesn't silently land on Crypto.
      if (sc.type !== "FEAR_GREED") sc.prevType = sc.type;
      sc.type = "FEAR_GREED";
      sc.layout = sc.layout === "CIRCULAR" ? "CIRCULAR" : "SLIDER";
      sc.symbol = sc.symbol === "stocks" ? "stocks" : "crypto";
      sc.symbol2 = "";
      sc.name2 = "";
      sc.name = fgName(sc);
      sc.ui = "edit";
      // The symbol above no longer names a priceable instrument - a leg this
      // session had checked (sc.px1/px2, sc.check1/2) would otherwise survive
      // untouched, since previewValue() never consults sc.symbol and pulls
      // sc.px1 whenever it's set. Left in place, a just-checked ticker price
      // leaks into buildStage() as the gauge's SCORE. Clear here, where the
      // symbol actually changes - not only removed on the way back out.
      // Clearing px1/px2 alone is not enough: previewValue()'s fromSaved1/2
      // fall back to the DEVICE's last poll (still the old instrument - the
      // device hasn't been told about this pending kind change, and won't be
      // until a save) whenever check1/check2 is falsy. "superseded" - not
      // undefined - keeps that fallback shut. checkLine() never reads it here
      // (its own type===FEAR_GREED check returns early), so no caption is at
      // stake, only previewValue()'s truthiness test.
      sc.px1 = null; sc.px2 = null; sc.check1 = "superseded"; sc.check2 = "superseded";
    } else {
      if (sc.type === "FEAR_GREED") {
        sc.type = sc.prevType || "CRYPTO";
        sc.symbol = "";
        sc.name = "";
        // Same reasoning in reverse: the FEAR_GREED symbol had no leg of its
        // own, but a stale check from BEFORE the FG switch could still be
        // sitting here from an earlier instrument. sc.symbol is now "" here,
        // so checkLine()'s !sc.symbol guard already blanks the caption - but
        // previewValue() doesn't consult sc.symbol, and would otherwise pull
        // the DEVICE's last poll for this srvId (still pricing the OLD
        // instrument, since the device only learns of the kind change on
        // save) under the now-blank ticker. "superseded", not undefined,
        // keeps that fallback shut until the next pick starts a real check.
        sc.px1 = null; sc.px2 = null; sc.check1 = "superseded"; sc.check2 = "superseded";
      }
      sc.layout = kind === "DUAL" ? "DUAL" : "SINGLE";
    }
    applyLayoutRules(sc);
    // applyLayoutRules() only RAISES an interval to its floor, so a pair's 2 min
    // would survive a switch to Single as a value with no pill lit, which reads
    // as a broken control - and the same is true in the other direction: an
    // hourly Fear & Greed poll (3600) is above either row's top and would
    // survive a switch to Dual just as unlit. Snap into the new row whenever
    // the value has left it. A plain min/max clamp against the row's ends is
    // not enough: a mid-row value like 600 (between 300 and 900) is already
    // within [choices[0], choices[last]], so it would clamp to itself and
    // still light no pill. Snap to whichever choice is nearest instead -
    // choices[0] is intervalFloor(sc) by construction, so the below-floor
    // case still lands exactly on the floor.
    const choices = isDual(sc) ? INTERVAL_CHOICES_DUAL : INTERVAL_CHOICES;
    if (choices.indexOf(sc.interval) === -1) {
      const v = sc.interval;
      sc.interval = choices.reduce((best, c) => (Math.abs(c - v) < Math.abs(best - v) ? c : best));
    }
    // A ticker with nothing to show has one sensible next step.
    if (sc.type !== "FEAR_GREED" && !sc.symbol) {
      sc.findLeg = 1;
      sc.ui = "find";
    }
    bindSheet();
    renderStrip();
    if (sc.ui === "find") $("fFind").focus();
  }

  // Caps the names to what fits the rendered title and raises a dual screen's
  // interval to its floor. Both are enforced by the firmware anyway - doing it
  // here turns a rejected save into something the user never sees.
  function applyLayoutRules(sc) {
    const max = nameMaxFor(sc);
    if (sc.name.length > max) sc.name = sc.name.slice(0, max);
    if (sc.name2.length > NAME_MAX.DUAL) sc.name2 = sc.name2.slice(0, NAME_MAX.DUAL);
    const floor = intervalFloor(sc);
    if (sc.interval < floor) sc.interval = floor;
  }

  function removeScreen() {
    const i = state.sel;
    state.screens.splice(i, 1);
    // The starred screen just went away, or everything after it shifted down.
    // When it is the starred one that goes, the star moves to whichever screen
    // takes the empty slot - the next one down, or the new last screen if the
    // deleted one was at the end. Same rule as the star button: the face always
    // has a screen driving it. Deleting the only screen is the one exception,
    // and Math.min lands on -1 there on its own.
    if (state.starIdx === i) state.starIdx = Math.min(i, state.screens.length - 1);
    else if (state.starIdx > i) state.starIdx -= 1;
    if (state.sel >= state.screens.length) state.sel = Math.max(0, state.screens.length - 1);
    closeSheet();
    renderStrip();
  }

  // ---------------------------------------------------------- Figo tab --

  // The device's own words for its four bands. BEARISH is labelled "Panicked"
  // because that is the expression it selects (Expression::Panicked) and the
  // name the settings ladder gives it - one vocabulary across both surfaces.
  const MOOD_LABEL = {
    BULLISH: "Bullish",
    HAPPY: "Happy",
    NEUTRAL: "Neutral",
    BEARISH: "Panicked",
    UNKNOWN: "Getting my bearings",
  };

  // The mood shown here is the DEVICE's, computed by the firmware from saved
  // config - not from unsaved edits sitting in this page. That is deliberate:
  // this tab reports what the face on the desk is actually doing.
  function moodWhy(mood) {
    if (!mood) return "";
    const sc = state.screens.find((s) => s.srvId === mood.screen_id);
    const name = sc ? sc.name || sc.symbol || "that screen" : null;
    const chg = mood.change_pct != null ? formatChange(mood.change_pct) : null;

    // The device reports the mood from SAVED config, so between a local
    // delete and the next save it can name a screen this page no longer
    // lists. Say that plainly rather than printing "null 24h change".
    if (mood.band !== "UNKNOWN" && (!sc || chg === null)) {
      return "Figo is still following the screen list it last saved. Save the carousel to bring the face up to date.";
    }

    if (mood.band === "BULLISH") {
      return (
        name + " 24h change " + chg + ", above your " + pct1(mood.bull_threshold) +
        " bullish mark, so the face on your desk has money in its eyes."
      );
    }
    if (mood.band === "HAPPY") {
      return (
        name + " 24h change " + chg + ", past your " + pct1(mood.happy_threshold) +
        " happy mark but not yet " + pct1(mood.bull_threshold) +
        ", so the face on your desk is quietly pleased."
      );
    }
    if (mood.band === "BEARISH") {
      return (
        name + " 24h change " + chg + ", below your " + pct1(mood.bear_threshold) +
        " panic mark, so the face on your desk is alarmed."
      );
    }
    if (mood.band === "NEUTRAL") {
      return (
        name + " 24h change " + chg + ", between your " + pct1(mood.bear_threshold) +
        " and " + pct1(mood.happy_threshold) +
        " marks, so the face on your desk is sitting this one out."
      );
    }
    // UNKNOWN covers four genuinely different situations, and the whole point
    // of having room for a sentence here is to say which one.
    if (!mood.screen_id) {
      return "No screen is driving the face yet. Star one on the Screens tab and Figo will follow it.";
    }
    if (!sc) {
      return "The screen that was driving the face is gone. Star another one on the Screens tab.";
    }
    const v = valueFor(sc);
    if (v && v.stale) {
      return (
        name + " has not refreshed lately, so Figo is holding a neutral face rather " +
        "than reacting to a number it no longer trusts."
      );
    }
    return "Figo has not fetched " + name + " yet. The face stays neutral until it has a number to react to.";
  }

  function renderFigoTab() {
    const setup = state.mode !== "STA";
    $("setupCard").hidden = !setup;
    $("connectedCard").hidden = setup;
    if (setup) {
      renderNetworks();
      return;
    }

    const mood = state.mood;
    $("moodLabel").textContent = mood ? MOOD_LABEL[mood.band] || MOOD_LABEL.UNKNOWN : "…";
    $("moodWhy").textContent = moodWhy(mood);

    const list = $("refreshList");
    list.textContent = "";
    state.screens.forEach((sc) => {
      const h = health(sc);
      const row = el("div", "fresh-row");
      row.appendChild(el("span", "dot7 " + HEALTH_BG[h.kind]));
      row.appendChild(el("span", "name", sc.name || sc.symbol || "Untitled"));
      row.appendChild(el("span", "age " + HEALTH_FG[h.kind], h.text));
      list.appendChild(row);
    });
    if (!state.screens.length) {
      list.appendChild(el("div", "net-empty", "No screens configured yet."));
    }
  }

  function renderNetworks() {
    const list = $("networkList");
    list.textContent = "";
    if (state.scanning) {
      list.appendChild(el("div", "net-empty", "Scanning…"));
      return;
    }
    if (!state.networks.length) {
      list.appendChild(el("div", "net-empty", "No networks found."));
      return;
    }
    state.networks.forEach((n) => {
      const b = el("button", "net-row");
      b.type = "button";
      b.appendChild(el("span", null, n.ssid + (n.secure ? " 🔒" : "")));
      b.appendChild(el("span", "rssi", n.rssi + " dBm"));
      b.addEventListener("click", () => {
        $("ssidInput").value = n.ssid;
        $("passInput").focus();
      });
      list.appendChild(b);
    });
  }

  // --------------------------------------------------------- Settings ----

  function renderSettings() {
    $("brightnessVal").textContent = state.brightness + "%";
    $("brightness").value = state.brightness;
    $("rot0").classList.toggle("on", state.rotation === 0);
    $("rot180").classList.toggle("on", state.rotation === 180);

    $("sleepToggle").setAttribute("aria-checked", state.sleepOn ? "true" : "false");
    $("sleepFields").hidden = !state.sleepOn;
    $("sleepSummary").textContent = state.sleepOn
      ? "Figo sleeps from " + state.sleepStart + " to " + state.sleepEnd + "."
      : "Display stays on around the clock.";
    $("sleepStart").value = state.sleepStart;
    $("sleepEnd").value = state.sleepEnd;

    const connected = state.mode === "STA";
    $("connectionBlock").hidden = !connected;
    if (connected) {
      $("infoSsid").textContent = state.status.ssid || "—";
      $("infoIp").textContent = state.status.ip || "—";
      $("infoFirmware").textContent = state.status.firmware
        ? "v" + state.status.firmware
        : "—";
    }

    // Derived, never an input: Neutral has no threshold of its own, it is
    // whatever the other two leave in the middle. Recomputed as they are typed
    // so the consequence of a change is visible before it is saved.
    $("neutralNote").textContent =
      "Neutral is everything in between, from " + pct1(state.bear) + " up to " +
      pct1(state.happy) + ".";

    $("btnAdv").textContent = (state.adv ? "▾" : "▸") + " Advanced";
    $("btnAdv").setAttribute("aria-expanded", state.adv ? "true" : "false");
    $("advBlock").hidden = !state.adv;
    // Last, and rendered from state: refresh() calls renderSettings() every
    // 10s, so this repaint is idempotent instead of a coin-flip about whether
    // it stomps an armed label.
    renderReset();
  }

  // --------------------------------------------------------- OTA update --
  // The phone reads the candidate firmware BEFORE offering to install it, so a
  // wrong file is refused here with a message a person can act on. The device
  // repeats every one of these checks as the bytes arrive (OtaImage.h) - this is
  // the friendly copy, that is the authority - so keep the two in step.
  const OTA_SLOT = 0x330000; // app slot size on the default_8MB table
  const OTA_VER_MAX = 15;
  // Spelled in two pieces so the whole marker never appears as a literal in
  // this file: app.js is compiled into the firmware (gzipped), and the image
  // scan must find exactly one marker - the real one.
  const OTA_MARKER = ["FIGO", "FW"].join("-") + ":";

  function otaInspect(u8) {
    if (u8.length < 14) return { error: "That file is too small to be firmware." };
    if (u8[0] !== 0xe9) return { error: "That isn't a firmware file. Pick Figo.ino.bin." };
    if ((u8[12] | (u8[13] << 8)) !== 0x000d) return { error: "That firmware is for a different chip." };
    // The Arduino IDE writes Figo.ino.merged.bin right beside the real file. It
    // has the same first bytes but carries the partition table at 0x8000.
    if (u8.length > 0x8001 && u8[0x8000] === 0xaa && u8[0x8001] === 0x50) {
      return { error: "That's the .merged.bin. Pick Figo.ino.bin instead." };
    }
    if (u8.length > OTA_SLOT) return { error: "That file is too large for Figo's update slot." };
    const n = OTA_MARKER.length;
    const c0 = OTA_MARKER.charCodeAt(0);
    for (let i = 0; i + n < u8.length; i++) {
      if (u8[i] !== c0) continue;
      let k = 1;
      while (k < n && u8[i + k] === OTA_MARKER.charCodeAt(k)) k++;
      if (k < n) continue;
      let v = "";
      let j = i + n;
      while (j < u8.length && v.length <= OTA_VER_MAX && u8[j] >= 0x20 && u8[j] <= 0x7e) {
        v += String.fromCharCode(u8[j++]);
      }
      if (j >= u8.length || u8[j] !== 0 || v.length === 0 || v.length > OTA_VER_MAX) {
        return { error: "That firmware's version number is unreadable." };
      }
      return { version: v };
    }
    return { error: "That's not a Figo firmware." };
  }

  // Dotted numeric compare: -1, 0, 1. Anything non-numeric compares as 0 in
  // that position, which is fine for a "this is older" note and nothing more.
  function verCmp(a, b) {
    const pa = String(a).split(/[^0-9]+/).filter(Boolean).map(Number);
    const pb = String(b).split(/[^0-9]+/).filter(Boolean).map(Number);
    for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
      const d = (pa[i] || 0) - (pb[i] || 0);
      if (d) return d < 0 ? -1 : 1;
    }
    return 0;
  }

  const ota = { file: null, version: null, busy: false };

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // A device that is mid-flash or rebooting can hold a request open, so every
  // poll here has its own deadline instead of trusting fetch's default.
  async function getJson(path, ms) {
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), ms || 4000);
    try {
      const res = await fetch(path, { cache: "no-store", signal: ctl.signal });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(data.error || res.statusText);
      return data;
    } finally {
      clearTimeout(t);
    }
  }

  function otaReset() {
    ota.file = null;
    ota.version = null;
    $("otaPlan").hidden = true;
    $("otaFile").value = "";
  }

  async function otaPicked() {
    const f = $("otaFile").files[0];
    if (!f) return;
    flash("otaMsg", "");
    let info;
    try {
      info = otaInspect(new Uint8Array(await f.arrayBuffer()));
    } catch (e) {
      info = { error: "Couldn't read that file." };
    }
    if (info.error) {
      otaReset();
      flash("otaMsg", info.error, true);
      return;
    }
    ota.file = f;
    ota.version = info.version;
    const running = state.status.firmware || "";
    const c = running ? verCmp(info.version, running) : 1;
    $("otaPlanText").textContent =
      "Install v" + info.version + (running ? " (running v" + running + ")" : "") + "?" +
      (c === 0 ? " That is the version already running." : c < 0 ? " That is older than the version running." : "") +
      " Figo will restart.";
    $("otaPlan").hidden = false;
  }

  // title/pct may be null to leave them as they are.
  function otaShow(title, status, pct, isErr) {
    $("otaCurtain").hidden = false;
    if (title) $("otaTitle").textContent = title;
    if (status != null) $("otaStatus").textContent = status;
    if (pct != null) $("otaFill").style.width = pct + "%";
    $("otaFill").classList.toggle("err", !!isErr);
  }

  function otaFail(message) {
    otaShow("Update didn't finish", message, 100, true);
    $("otaHint").hidden = true;
    $("btnOtaClose").hidden = false;
    $("btnOtaClose").focus();
  }

  function otaClose() {
    $("otaCurtain").hidden = true;
    $("btnOtaClose").hidden = true;
    $("otaHint").hidden = false;
    otaReset();
    ota.busy = false;
    clearInterval(state.pollTimer);
    state.pollTimer = setInterval(refresh, 10000);
    refresh();
  }

  // XHR, not fetch(): fetch cannot report UPLOAD progress. Sends the raw file
  // as the body, which is what the device's body callback streams to flash.
  function otaUpload(file, token) {
    return new Promise((resolve, reject) => {
      const x = new XMLHttpRequest();
      x.open("POST", "/api/ota/upload?token=" + encodeURIComponent(token));
      x.setRequestHeader("Content-Type", "application/octet-stream");
      x.timeout = 5 * 60 * 1000;
      x.upload.onprogress = (e) => {
        if (!e.lengthComputable) return;
        const p = Math.floor((e.loaded / e.total) * 100);
        otaShow(null, "Sending firmware… " + p + "%", p);
      };
      x.onload = () => {
        let j = {};
        try { j = JSON.parse(x.responseText); } catch (e) { /* not JSON */ }
        if (x.status === 200) resolve();
        else reject(new Error(j.error || "The upload failed (" + x.status + ")."));
      };
      x.onerror = () => reject(new Error("Lost the connection to Figo during the upload."));
      x.ontimeout = () => reject(new Error("The upload timed out."));
      x.onabort = () => reject(new Error("The upload was interrupted."));
      x.send(file);
    });
  }

  // After the device says "done" it reboots into the new image, which is on
  // probation until it has proved itself. Success is the NEW version reporting
  // ota_state "valid" - not merely answering. Going back to the OLD version
  // after having been down is the rollback, and is reported as one.
  async function otaAwaitReturn(target, was) {
    otaShow(null, "Figo is restarting…", 100);
    const t0 = Date.now();
    let sawDown = false;
    let sawPending = false;
    await sleep(2000);
    while (Date.now() - t0 < 4 * 60 * 1000) {
      let s = null;
      try {
        s = await getJson("/api/status");
      } catch (e) {
        sawDown = true;
        await sleep(1500);
        continue;
      }
      const fw = s.firmware || "";
      const valid = s.ota_state === "valid";
      if (fw === target && s.ota_state === "pending_verify") {
        sawPending = true;
        otaShow(null, "Figo restarted. Checking the new firmware…", 100);
      } else if (fw === target && valid && (target !== was || sawPending)) {
        return;
      } else if (fw === was && target !== was && sawDown && valid) {
        throw new Error("Figo went back to v" + was + ". The new firmware didn't start properly, so nothing was changed.");
      }
      await sleep(1500);
    }
    throw new Error("Figo hasn't come back yet. Check its screen, then reload this page.");
  }

  async function otaInstall() {
    if (ota.busy || !ota.file) return;
    ota.busy = true;
    const file = ota.file;
    const target = ota.version;
    const was = state.status.firmware || "";
    // The 10s refresh would compete with the upload for a device that is being
    // asked to do one thing, and would collect 503s besides.
    clearInterval(state.pollTimer);
    closeSheet();
    $("otaHint").hidden = false;
    $("btnOtaClose").hidden = true;
    otaShow("Updating Figo", "Getting Figo ready…", 0);
    // Keeps the phone's screen on so a sleeping tab does not drop the upload.
    // NOT awaited: request() can stay pending indefinitely when the page is not
    // visible, and this is a nicety that must never gate the update itself.
    let wake = null;
    let over = false;
    if (navigator.wakeLock) {
      navigator.wakeLock.request("screen").then(
        (w) => { if (over) w.release().catch(() => {}); else wake = w; },
        () => { /* optional */ }
      );
    }
    try {
      const begin = await apiPost("/api/ota/begin", {});
      // "ready" means Figo has stopped its own network fetches, which can take
      // as long as one in flight.
      let st = {};
      for (let i = 0; i < 60; i++) {
        st = await getJson("/api/ota/status");
        if (st.state === "ready" || st.state === "failed") break;
        await sleep(500);
      }
      if (st.state !== "ready") throw new Error(st.error || "Figo didn't get ready in time. Try again.");

      otaShow(null, "Sending firmware… 0%", 0);
      await otaUpload(file, begin.token);

      otaShow(null, "Checking the update…", 100);
      for (let i = 0; i < 120; i++) {
        st = await getJson("/api/ota/status");
        if (st.state === "done" || st.state === "failed") break;
        await sleep(500);
      }
      if (st.state === "failed") throw new Error(st.error || "Figo rejected the update.");
      if (st.state !== "done") throw new Error("Figo didn't confirm the update.");

      await otaAwaitReturn(target, was);
      otaShow("Figo is updated", "Now running v" + target + ".", 100);
      setTimeout(() => location.reload(), 1200);
    } catch (e) {
      otaFail(e.message);
    } finally {
      over = true;
      if (wake) wake.release().catch(() => {});
    }
  }

  // ------------------------------------------------------- factory reset --

  const RESET_ARM_MS = 5000;

  function renderReset() {
    const armed = state.resetArmed;
    const btn = $("btnReset");
    btn.textContent = armed ? "Tap again to erase everything" : "Factory reset";
    btn.classList.toggle("armed", armed);
    $("btnResetCancel").hidden = !armed;
  }

  function armReset() {
    state.resetArmed = true;
    clearTimeout(state.resetTimer);
    state.resetTimer = setTimeout(disarmReset, RESET_ARM_MS);
    renderReset();
  }

  function disarmReset() {
    state.resetArmed = false;
    clearTimeout(state.resetTimer);
    state.resetTimer = null;
    renderReset();
  }

  // Opaque and non-dismissible: after a full wipe the device is on Figo-Setup
  // and this phone is still on the house WiFi, so the page never recovers -
  // and in AP mode the portal returns at the same address with no evidence
  // anything happened. Nothing behind the curtain is true any more.
  function showResetCurtain() {
    clearInterval(state.pollTimer);
    disarmReset();
    closeSheet();
    const c = $("resetCurtain");
    c.hidden = false;
    c.setAttribute("tabindex", "-1");
    c.focus();
  }

  // Seeds the three threshold inputs. Called from loadConfig() only, for the
  // reason spelled out in renderSettings().
  function renderFace() {
    $("fBull").value = state.bull;
    $("fHappy").value = state.happy;
    $("fBear").value = state.bear;
  }

  // 96 entries, 00:00 to 23:45. A native select rather than a free-text time
  // field: ConfigManager::updateSystem() validates a strict "HH:MM" and a
  // typo there comes back as a 400 the user cannot act on.
  function fillTimeOptions() {
    const html = [];
    for (let m = 0; m < 24 * 60; m += 15) {
      const h = Math.floor(m / 60);
      const mm = m % 60;
      const t = (h < 10 ? "0" : "") + h + ":" + (mm < 10 ? "0" : "") + mm;
      html.push(t);
    }
    ["sleepStart", "sleepEnd"].forEach((id) => {
      const sel = $(id);
      sel.textContent = "";
      html.forEach((t) => {
        const o = document.createElement("option");
        o.value = t;
        o.textContent = t;
        sel.appendChild(o);
      });
    });
  }

  // ------------------------------------------------------------- tabs ----

  function goTab(name) {
    state.tab = name;
    disarmReset();
    // Switching tabs closes the sheet - it belongs to the Screens tab and
    // would otherwise float over an unrelated page.
    closeSheet();
    document.querySelectorAll(".tab-panel").forEach((p) => p.classList.remove("active"));
    $("tab-" + name).classList.add("active");
    document.querySelectorAll(".tab-btn").forEach((b) => {
      b.classList.toggle("on", b.dataset.tab === name);
    });
    // The app frame is locked (body overflow:hidden) and each panel scrolls
    // internally, so the old window.scrollTo(0,0) is inert - reset the panel
    // itself to keep the "every tab opens at the top" behaviour.
    $("tab-" + name).scrollTop = 0;
  }

  // ------------------------------------------------------------ saving --

  // Both "Save carousel" and "Save face" write the expression_source section,
  // because the star lives on one tab and its thresholds on another while the
  // device stores them together. Built in one place so neither can send half a
  // section and blank the other half. The consequence worth knowing: saving
  // either one commits what is currently on screen for both.
  function expressionPayload(ids) {
    return {
      // The star is held as an index, so it has to be resolved through the
      // SAME id list collectScreens() sends, in the same save - see
      // assignIds(). Reading it as starIdx + 1, which is what this did while
      // ids were positional, now points the face at whatever screen happens
      // to sit in that slot.
      screen_id: state.starIdx >= 0 ? ids[state.starIdx] : 0,
      bull_threshold: state.bull,
      happy_threshold: state.happy,
      bear_threshold: state.bear,
    };
  }

  // Mirrors ConfigManager::updateExpressionSource()'s two checks, so a bad
  // ladder is caught before a round trip and names the field to move.
  function thresholdError() {
    if (!(state.bull > state.happy)) return "Bullish must sit above Happy.";
    if (!(state.happy > state.bear)) return "Happy must sit above Panicked.";
    return null;
  }

  // The ids this save will send, one per screen, in list order.
  //
  // An id is the key a screen's market data is cached under on the device:
  // DataFetcher::syncStatesToConfig() looks the previous state up by id and
  // keeps the cached price when identityMatches() says the screen still
  // points at the same instrument. Both lookups are by-match, not by-index,
  // so ids do NOT have to be contiguous or in order - which is what makes
  // this fix possible.
  //
  // Renumbering by position (id = i + 1) would make a reorder refetch the
  // whole carousel: after a swap, id 1 names a different
  // instrument, identityMatches() correctly says "not the same thing",
  // and every moved screen is blanked to "--" and refetched. srvId travels
  // WITH the screen object through moveScreen(), so simply keeping it is the
  // whole repair - a pure reorder now sends the same ids attached to the same
  // instruments, and nothing on the device notices anything changed.
  //
  // Only a screen the device has never seen (srvId null) needs an id, and it
  // takes the lowest one nobody else is using. Ids must stay inside
  // 1..MAX_SCREENS: updateExpressionSource() validates screen_id against that
  // range, so an id of 9 would make a starred screen unstarrable.
  function assignIds() {
    const used = new Set();
    // Pass one keeps every id the device already gave us. The !used.has()
    // guard is belt and braces - the device hands out unique ids - but a
    // duplicate slipping through would alias two screens onto one cache slot,
    // which is a far worse bug than a refetch.
    const ids = state.screens.map((s) => {
      const id = s.srvId;
      if (id != null && id >= 1 && id <= MAX_SCREENS && !used.has(id)) {
        used.add(id);
        return id;
      }
      return 0; // filled in below, once every kept id is known
    });
    // Pass two fills the gaps, lowest free id first.
    let next = 1;
    for (let i = 0; i < ids.length; i++) {
      if (ids[i] !== 0) continue;
      while (used.has(next)) next++;
      ids[i] = next;
      used.add(next);
    }
    return ids;
  }

  function collectScreens(ids) {
    return state.screens.map((s, i) => ({
      id: ids[i],
      type: s.type,
      symbol: s.symbol.trim(),
      name: s.name.trim(),
      interval_sec: s.interval,
      layout: s.layout,
      symbol2: s.symbol2.trim(),
      name2: s.name2.trim(),
    }));
  }

  // What ConfigManager::updateScreens() would reject, checked HERE so the
  // phone can name the screen. The device refuses the WHOLE payload on the
  // first bad screen and its message names none, so without this a single
  // half-built pair blocks every save with a sentence the user cannot place.
  // Lengths are BYTES, as the firmware's strlen() counts them; a symbol needs
  // room for the terminator in its 16-byte buffer, so 15 at most.
  // Returns { text, leg } (leg: which half to open the search on) or null.
  const byteLen = (s) => new TextEncoder().encode(s).length;
  const SYMBOL_BYTES_MAX = 15;
  function screenProblem(sc, i) {
    const n = "Screen " + (i + 1);
    const sym = sc.symbol.trim();
    if (!sym) return { text: n + " has no instrument yet. Pick one, or remove the screen.", leg: 1 };
    if (byteLen(sym) > SYMBOL_BYTES_MAX) {
      return { text: n + "'s symbol is longer than Figo can hold (" + SYMBOL_BYTES_MAX + " characters).", leg: 1 };
    }
    if (sc.type !== "FEAR_GREED") {
      const name = sc.name.trim();
      if (!name) return { text: n + " needs a name.", leg: 0 };
      if (byteLen(name) > nameMaxFor(sc)) {
        return { text: n + "'s name is too long for its layout (" + nameMaxFor(sc) + " characters).", leg: 0 };
      }
    }
    if (isDual(sc)) {
      const sym2 = sc.symbol2.trim();
      if (!sym2) {
        return { text: n + " is a pair with no second instrument. Pick one, or switch it to Single.", leg: 2 };
      }
      if (byteLen(sym2) > SYMBOL_BYTES_MAX) {
        return { text: n + "'s second symbol is longer than Figo can hold (" + SYMBOL_BYTES_MAX + " characters).", leg: 2 };
      }
      const name2 = sc.name2.trim();
      if (!name2) return { text: n + " needs a name for its second half.", leg: 0 };
      if (byteLen(name2) > NAME_MAX.DUAL) {
        return { text: n + "'s second name is too long (" + NAME_MAX.DUAL + " characters).", leg: 0 };
      }
    }
    return null;
  }

  // Takes the user to the screen that is wrong, and to the half that needs
  // filling: the message alone is easy to lose behind the editor.
  function showScreenProblem(i, p) {
    const sc = state.screens[i];
    state.sel = i;
    renderStrip();
    centerFrame(i);
    sc.ui = "edit";
    openSheet();
    if (p.leg) {
      goFind(p.leg);
      paintFindStatus(null, p.text);
    }
    flash("screensMsg", p.text, true);
  }

  // Legs the device said it could not price (not "unknown": an interrupted or
  // unreachable check says nothing about the symbol, so it never counts here).
  function unpriceableLegs() {
    const out = [];
    state.screens.forEach((sc, i) => {
      if (sc.type === "FEAR_GREED") return;
      if (sc.check1 === "failed") out.push("screen " + (i + 1) + " (" + sc.symbol + ")");
      if (isDual(sc) && sc.check2 === "failed") out.push("screen " + (i + 1) + " (" + sc.symbol2 + ")");
    });
    return out;
  }

  async function saveScreens() {
    if (!state.screens.length) {
      flash("screensMsg", "Add at least one screen", true);
      return;
    }
    for (let i = 0; i < state.screens.length; i++) {
      const p = screenProblem(state.screens[i], i);
      if (p) {
        showScreenProblem(i, p);
        return;
      }
    }
    // A pick the device could not price still saves, and then shows a blank
    // frame on the desk indefinitely. Not blocked - the check can fail for
    // reasons that are not the symbol's fault (a rate limit, a slow answer) -
    // but never silent either.
    const bad = unpriceableLegs();
    if (
      bad.length &&
      !confirm(
        "Figo couldn't price " + bad.join(", ") +
          ". A screen it can't price stays blank on the panel.\n\nSave anyway?"
      )
    ) {
      return;
    }
    try {
      // One id list for both POSTs, computed once: the star is sent as an id,
      // and it has to be the id the screens payload actually gave that screen.
      const ids = assignIds();
      // Two POSTs, in this order and strictly sequential. handlePostConfig()
      // matches exactly one section per request, and the star lives in
      // expression_source while the frames live in screens - so the ids the
      // star refers to have to be written first.
      await apiPost("/api/config", { screens: collectScreens(ids) });
      await apiPost("/api/config", { expression_source: expressionPayload(ids) });
      flash("screensMsg", "Saved, the carousel updates live, no reboot needed.");
      await loadConfig();
      await loadScreenState();
      renderStrip();
      renderFigoTab();
    } catch (e) {
      flash("screensMsg", e.message, true);
    }
  }

  async function saveFace() {
    const bad = thresholdError();
    if (bad) {
      flash("faceMsg", bad, true);
      return;
    }
    try {
      // Same id list the Screens tab would send, so the star resolves to the
      // same screen whichever tab saves it. A star sitting on a screen that
      // has never been saved resolves to an id the device does not hold yet -
      // a dangling id, which moodFor() already defines as Unknown rather than
      // an error, and which the next Save on the Screens tab makes real.
      await apiPost("/api/config", { expression_source: expressionPayload(assignIds()) });
      flash("faceMsg", "Saved, Figo's face follows the new bands.");
    } catch (e) {
      flash("faceMsg", e.message, true);
    }
  }

  // Minutes EAST of UTC, which is the sign the firmware stores. The browser
  // reports minutes BEHIND UTC, hence the negation. Read fresh on every call
  // rather than cached at load: the value is DST-aware at the moment it is
  // read, and that is the entire mechanism by which Figo's sleep window
  // survives a DST shift without a zone database on the device.
  const phoneOffsetMin = () => -new Date().getTimezoneOffset();

  async function saveSystem({ silent = false } = {}) {
    try {
      await apiPost("/api/config", {
        system: {
          brightness: state.brightness,
          rotation: state.rotation,
          // Sleep off means both fields empty - that is how the firmware
          // stores "always on", not a 00:00-00:00 window.
          sleep_start: state.sleepOn ? state.sleepStart : "",
          sleep_end: state.sleepOn ? state.sleepEnd : "",
          utc_offset_min: phoneOffsetMin(),
        },
      });
      if (!silent) flash("systemMsg", "Saved.");
    } catch (e) {
      // A silent correction that fails is not the user's problem to solve -
      // the window is simply an hour out until the next load, which is the
      // tolerance this design already accepts.
      if (!silent) flash("systemMsg", e.message, true);
    }
  }

  async function saveProviders() {
    const providers = {
      yahoo_host: $("hostYahoo").value.trim(),
      coingecko_host: $("hostCg").value.trim(),
      fng_crypto_host: $("hostFngC").value.trim(),
      fng_stocks_host: $("hostFngS").value.trim(),
    };
    // The device never sends the stored key back (it is write-only), so an
    // untouched field is empty and must be OMITTED rather than sent as "" -
    // sending "" is how you deliberately go keyless, and would otherwise wipe
    // a stored key on every unrelated save.
    if (state.cgKeyDirty) providers.coingecko_key = $("cgKey").value.trim();
    try {
      await apiPost("/api/config", { providers });
      state.cgKeyDirty = false;
      flash("providersMsg", "Saved.");
      await loadConfig();
    } catch (e) {
      flash("providersMsg", e.message, true);
    }
  }

  // ------------------------------------------------------------- WiFi ----

  async function scanNetworks() {
    state.scanning = true;
    $("btnScan").textContent = "Scanning…";
    renderNetworks();
    try {
      const nets = await apiGet("/api/wifi/scan");
      state.networks = nets.sort((a, b) => b.rssi - a.rssi);
    } catch (e) {
      state.networks = [];
    }
    state.scanning = false;
    $("btnScan").textContent = "Scan again";
    renderNetworks();
  }

  async function connectWifi() {
    const ssid = $("ssidInput").value.trim();
    if (!ssid) {
      flash("wifiMsg", "Pick a network or type its name", true);
      return;
    }
    // The device blocks its HTTP response until the STA association resolves
    // (up to WIFI_STA_CONNECT_TIMEOUT_MS, and a good network is still often
    // 6-7s), so this button must stay visibly busy for the whole wait -
    // flash()'s normal 2.8s auto-clear would let "Connecting…" vanish long
    // before the request settles, reading as a dropped tap and inviting a
    // second, overlapping /api/wifi/connect.
    const btn = $("btnConnect");
    btn.disabled = true;
    btn.textContent = "Connecting…";
    clearTimeout(msgTimers["wifiMsg"]);
    const msgNode = $("wifiMsg");
    msgNode.textContent = "Connecting… this can take several seconds";
    msgNode.classList.remove("err");
    try {
      await apiPost("/api/wifi/connect", { ssid: ssid, password: $("passInput").value });
      flash("wifiMsg", "Connected! Find me at figo.local");
      // The SoftAP drops a few seconds after a successful switch, so the phone
      // is about to lose this page. Refresh what we can while it is still up.
      setTimeout(refresh, 1500);
    } catch (e) {
      flash("wifiMsg", e.message, true);
    } finally {
      btn.disabled = false;
      btn.textContent = "Connect";
    }
  }

  // ------------------------------------------------------------- load ----

  async function loadStatus() {
    const s = await apiGet("/api/status");
    state.status = s;
    state.mode = s.mode;
  }

  async function loadConfig() {
    const cfg = await apiGet("/api/config");

    state.screens = (cfg.screens || []).map((s) => ({
      srvId: s.id,
      type: s.type || "CRYPTO",
      symbol: s.symbol || "",
      name: s.name || "",
      interval: s.interval_sec || INTERVAL_MIN,
      layout: s.layout || (s.type === "FEAR_GREED" ? "SLIDER" : "SINGLE"),
      symbol2: s.symbol2 || "",
      name2: s.name2 || "",
    }));
    if (state.sel >= state.screens.length) {
      state.sel = Math.max(0, state.screens.length - 1);
    }

    const expr = cfg.expression_source || {};
    state.bull = expr.bull_threshold != null ? expr.bull_threshold : 5.0;
    state.happy = expr.happy_threshold != null ? expr.happy_threshold : 1.0;
    state.bear = expr.bear_threshold != null ? expr.bear_threshold : -2.5;
    renderFace();
    // Stored as an id, held as an index. A dangling id (the starred screen was
    // deleted) resolves to -1, which is the same "nothing starred" state the
    // firmware reads as MoodBand::Unknown.
    const starId = expr.screen_id || 0;
    state.starIdx = starId ? state.screens.findIndex((s) => s.srvId === starId) : -1;

    state.brightness = cfg.system.brightness;
    state.rotation = Number(cfg.system.rotation) || 0;
    const ss = cfg.system.sleep_start || "";
    const se = cfg.system.sleep_end || "";
    state.sleepOn = !!(ss && se);
    if (ss) state.sleepStart = ss;
    if (se) state.sleepEnd = se;

    // Self-healing timezone. The stored offset was correct when it was written;
    // a DST shift (or a device provisioned from another timezone) makes it
    // wrong, and the browser's own offset is authoritative and DST-aware right
    // now. Written back only on an actual mismatch - twice a year plus once at
    // upgrade - because a system POST bumps g_configGen, which rebuilds the
    // data screens (ScreenManager.h). A write on every page load would flicker
    // the carousel every time the phone UI is opened, and churn NVS for nothing.
    const storedOffset = Number(cfg.system.utc_offset_min) || 0;
    if (storedOffset !== phoneOffsetMin()) saveSystem({ silent: true });

    const p = cfg.providers || {};
    $("hostYahoo").value = p.yahoo_host || "";
    $("hostCg").value = p.coingecko_host || "";
    $("hostFngC").value = p.fng_crypto_host || "";
    $("hostFngS").value = p.fng_stocks_host || "";
    $("cgKey").value = "";
    state.cgKeyDirty = false;
    $("cgKeyState").textContent = p.coingecko_key_configured
      ? "A key is saved. Type a new one to replace it, or clear this field and save to go keyless."
      : "No key saved — Figo is using CoinGecko's keyless endpoint.";
  }

  async function loadScreenState() {
    const st = await apiGet("/api/screens/state");
    const byId = {};
    (st.screens || []).forEach((s) => {
      byId[s.id] = s;
    });
    state.values = byId;
    state.mood = st.mood || null;
  }

  // The 10s poll. Deliberately skipped while a frame is being dragged: a
  // rebuild mid-gesture would swap the DOM out from under the finger.
  async function refresh() {
    if (state.dragIdx !== null) return;
    try {
      await loadStatus();
    } catch (e) {
      return; // Device unreachable - keep showing the last good state.
    }
    if (state.mode === "STA") {
      try {
        await loadScreenState();
      } catch (e) {
        // 503 means the state mutex was briefly busy; the next tick will do.
      }
    }
    renderFigoTab();
    renderSettings();
    refreshStages();
    // Guarded: the selected screen can have been removed since the sheet was
    // opened, and health() has no meaning without one.
    if (state.sheetOpen && screenAt(state.sel)) {
      $("sheetAge").textContent = health(screenAt(state.sel)).text;
    }
  }

  // ------------------------------------------------------------ wiring --

  function wire() {
    document.querySelectorAll(".tab-btn").forEach((b) => {
      b.addEventListener("click", () => goTab(b.dataset.tab));
    });

    // Figo tab
    $("btnScan").addEventListener("click", scanNetworks);
    $("btnConnect").addEventListener("click", connectWifi);

    // Screens tab
    $("btnSaveScreens").addEventListener("click", saveScreens);
    const strip = $("strip");
    // Passive: this only ever toggles classes, and a non-passive listener on
    // the strip's own scroll would put the main thread back in the way of a
    // compositor-driven fling - the exact thing the native-browsing model
    // exists to keep out of.
    strip.addEventListener("scroll", syncSelToScroll, { passive: true });
    strip.addEventListener("pointermove", onStripMove);
    strip.addEventListener("pointerup", onStripUp);
    strip.addEventListener("pointercancel", onStripUp);
    strip.addEventListener("pointerleave", onStripLeave);
    // The load-bearing line of the whole gesture model. While a frame is
    // held, preventDefault on touchmove stops the browser from starting its
    // native pan - which would kill the drag with pointercancel - while a
    // plain browse keeps full native scrolling, inertia and snap because the
    // guard never fires for it. It MUST be non-passive (a passive listener's
    // preventDefault is silently ignored) and MUST be registered before the
    // touch begins. touch-action cannot do this job: it is latched at
    // touchstart, before anyone knows whether the touch is a browse or a
    // hold. JS-driven swiping was tried instead and reverted: pointermove ->
    // scrollLeft tracking runs on the main thread and visibly trails the
    // finger on iOS, where native panning is compositor-driven.
    strip.addEventListener(
      "touchmove",
      (e) => {
        if (state.dragIdx !== null) e.preventDefault();
      },
      { passive: false }
    );

    // Sheet
    $("backdrop").addEventListener("click", closeSheet);
    $("btnDone").addEventListener("click", closeSheet);
    $("btnRemove").addEventListener("click", removeScreen);
    $("btnFindBack").addEventListener("click", () => {
      setMode("edit");
      recheckInterrupted(screenAt(state.sel));
    });
    $("fFind").addEventListener("input", () => {
      clearTimeout(findTimer);
      findTimer = setTimeout(() => runFind($("fFind").value), 350);
    });
    [
      ["fName", "name"],
      ["fName2", "name2"],
    ].forEach(([id, key]) => {
      $(id).addEventListener("input", () => {
        const sc = screenAt(state.sel);
        sc[key] = $(id).value;
        // The sheet's own preview is the thing being named, so repaint it as
        // the name is typed. paintPreview(), NOT bindSheet(): rewriting the
        // input's value mid-typing throws the caret to the end.
        if (state.sheetOpen && sc.ui !== "find") paintPreview(sc);
        renderStrip();
      });
    });

    // Settings
    $("brightness").addEventListener("input", (e) => {
      state.brightness = parseInt(e.target.value, 10);
      $("brightnessVal").textContent = state.brightness + "%";
    });
    $("rot0").addEventListener("click", () => {
      state.rotation = 0;
      renderSettings();
    });
    $("rot180").addEventListener("click", () => {
      state.rotation = 180;
      renderSettings();
    });
    $("sleepToggle").addEventListener("click", () => {
      state.sleepOn = !state.sleepOn;
      renderSettings();
    });
    $("sleepStart").addEventListener("change", () => {
      state.sleepStart = $("sleepStart").value;
      renderSettings();
    });
    $("sleepEnd").addEventListener("change", () => {
      state.sleepEnd = $("sleepEnd").value;
      renderSettings();
    });
    $("btnSaveSystem").addEventListener("click", () => saveSystem());

    [["fBull", "bull"], ["fHappy", "happy"], ["fBear", "bear"]].forEach(([id, key]) => {
      $(id).addEventListener("input", () => {
        const v = parseFloat($(id).value);
        // A half-typed "-" or an emptied field parses to NaN; hold the last
        // good number rather than writing NaN into state and then a payload.
        if (!isNaN(v)) state[key] = v;
        renderSettings();
      });
    });
    $("btnSaveFace").addEventListener("click", saveFace);

    $("brightToggle").addEventListener("click", () => {
      const next = document.documentElement.getAttribute("data-theme") !== "bright";
      applyTheme(next);
      try {
        localStorage.setItem("figo-bright", next ? "1" : "0");
      } catch (e) {
        // Preference simply will not persist; the page still themes correctly.
      }
    });

    $("btnAdv").addEventListener("click", () => {
      state.adv = !state.adv;
      if (!state.adv) disarmReset();
      renderSettings();
    });
    $("cgKey").addEventListener("input", () => {
      state.cgKeyDirty = true;
    });
    $("btnSaveProviders").addEventListener("click", saveProviders);

    $("btnForget").addEventListener("click", async () => {
      if (!confirm("Forget this WiFi network? Figo will restart into setup mode.")) return;
      try {
        await apiPost("/api/wifi/forget", {});
        flash("systemMsg", "Forgetting network and restarting…");
      } catch (e) {
        flash("systemMsg", e.message, true);
      }
    });
    $("btnReset").addEventListener("click", async () => {
      if (!state.resetArmed) {
        armReset();
        return;
      }
      const btn = $("btnReset");
      disarmReset();
      btn.disabled = true;
      flash("resetMsg", "");
      try {
        await apiPost("/api/factory-reset", {});
        showResetCurtain();
      } catch (e) {
        // A TypeError is fetch's own "the connection went away", which is
        // indistinguishable from the device dropping the link a moment early:
        // treat it as done. A plain Error means a device that is still there
        // answered with a status (apiPost throws that only on !res.ok).
        if (e instanceof TypeError) {
          showResetCurtain();
          return;
        }
        btn.disabled = false;
        flash("resetMsg", e.message, true);
      }
    });
    $("btnResetCancel").addEventListener("click", disarmReset);
    $("btnOtaPick").addEventListener("click", () => $("otaFile").click());
    $("otaFile").addEventListener("change", otaPicked);
    $("btnOtaGo").addEventListener("click", otaInstall);
    $("btnOtaCancel").addEventListener("click", otaReset);
    $("btnOtaClose").addEventListener("click", otaClose);
    $("btnReboot").addEventListener("click", async () => {
      if (!confirm("Reboot Figo now?")) return;
      try {
        await apiPost("/api/reboot", {});
        flash("systemMsg", "Rebooting…");
      } catch (e) {
        flash("systemMsg", e.message, true);
      }
    });
  }

  async function init() {
    loadTheme();
    fillTimeOptions();
    wire();

    try {
      await loadStatus();
    } catch (e) {
      state.mode = "AP";
    }
    try {
      await loadConfig();
    } catch (e) {
      // Leaves the page on its defaults; the Figo tab still reports the link.
    }
    if (state.mode === "STA") {
      try {
        await loadScreenState();
      } catch (e) {
        // The endpoint is new - an older firmware simply has no freshness to
        // report, and every row falls back to "waiting for the first fetch".
      }
    }

    // Screens is the useful landing tab once Figo is on the network; before
    // that, the only thing worth showing is how to get it there.
    goTab(state.mode === "STA" ? "screens" : "status");
    renderStrip();
    renderFigoTab();
    renderSettings();
    if (state.mode !== "STA") scanNetworks();

    state.pollTimer = setInterval(refresh, 10000);
  }

  init();
})();
