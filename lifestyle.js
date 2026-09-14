// Four everyday apps: weather, a calendar, the appliances and the plants. Content sits on tonal surfaces;
// presses answer on pointer-down (press feedback pattern), the week strip follows the finger 1:1 and pages on
// Detent's paging pattern, and every value that moves hands its velocity to a spring. All data is example data.

import { pressSurface } from './controls.js';
import { Motion, SPRINGS, project, rubberBand, velocityTracker } from './motion.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const pad = (n) => String(n).padStart(2, '0');
const hm = (d = new Date()) => `${pad(d.getHours())}:${pad(d.getMinutes())}`;
const none = { x: 0, y: 0, w: 0, h: 0, r: 0 };
const DAY = 86400000;
const WEEKDAYS = ['sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday'];
const MONTHS = ['january', 'february', 'march', 'april', 'may', 'june', 'july', 'august', 'september', 'october', 'november', 'december'];
/** Days since 1970 for a local date, counted in UTC so daylight saving never moves a day. */
const dayNumber = (d = new Date()) => Math.floor(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / DAY);
const dateOf = (n) => { const u = new Date(n * DAY); return new Date(u.getUTCFullYear(), u.getUTCMonth(), u.getUTCDate()); };
const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;

// ---------- weather ----------
// The house model: the overview, today and standby all say 18° and partly cloudy, so the app says so too.
const NOW = { temp: 18, text: 'partly cloudy', icon: 'partly_cloudy_day' };
// The next twelve hours from now, as today's page shows them, with each hour's chance of rain.
const NEXT = { temp: [18, 17, 16, 15, 14, 14, 13, 12, 12, 11, 11, 11], rain: [10, 20, 40, 60, 70, 70, 60, 60, 50, 50, 60, 70] };
// Seven days from today: low, high, a condition and rain windows as [from hour, to hour, chance].
const WEEK = [
  { icon: 'partly_cloudy_day', text: 'partly cloudy', lo: 12, hi: 21, rain: [] },
  { icon: 'rainy', text: 'showers', lo: 11, hi: 17, rain: [[6, 13, 80]] },
  { icon: 'cloud', text: 'cloudy', lo: 10, hi: 16, rain: [[15, 17, 30]] },
  { icon: 'partly_cloudy_day', text: 'partly cloudy', lo: 9, hi: 19, rain: [] },
  { icon: 'sunny', text: 'sunny', lo: 11, hi: 23, rain: [] },
  { icon: 'sunny', text: 'sunny', lo: 13, hi: 24, rain: [] },
  { icon: 'thunderstorm', text: 'storms', lo: 14, hi: 20, rain: [[14, 19, 60]] },
];
const SUNNY = new Set(['sunny', 'partly_cloudy_day']);
const WEEK_LO = Math.min(...WEEK.map((d) => d.lo));
const WEEK_HI = Math.max(...WEEK.map((d) => d.hi));

// Coldest at five, warmest at three, and a cosine between them, the way a day's temperature roughly runs.
function tempAt(day, h) {
  const { lo, hi } = WEEK[day];
  if (h >= 5 && h <= 15) return lo + ((hi - lo) * (1 - Math.cos((Math.PI * (h - 5)) / 10))) / 2;
  const t = h > 15 ? (h - 15) / 14 : (h + 9) / 14;
  return lo + ((hi - lo) * (1 + Math.cos(Math.PI * t))) / 2;
}
function rainAt(day, h) {
  let chance = 0;
  for (const [from, to, p] of WEEK[day].rain) {
    if (h >= from && h <= to) chance = Math.max(chance, p);
    else if (h === from - 1 || h === to + 1) chance = Math.max(chance, Math.round((p * 0.4) / 10) * 10);
  }
  return chance;
}
/** Twelve hours: from now for today, the daytime 08–19 for any other day. */
function hoursFor(day, now) {
  if (day === 0) {
    const h0 = now.getHours();
    return NEXT.temp.map((temp, k) => ({ hour: (h0 + k) % 24, temp, rain: NEXT.rain[k], now: k === 0 }));
  }
  return Array.from({ length: 12 }, (_, k) => ({ hour: 8 + k, temp: Math.round(tempAt(day, 8 + k)), rain: rainAt(day, 8 + k), now: false }));
}
const rainOf = (day) => (day === 0 ? Math.max(...NEXT.rain) : Math.max(...Array.from({ length: 24 }, (_, h) => rainAt(day, h))));
const dayName = (day, now) => (day === 0 ? 'today' : day === 1 ? 'tomorrow' : WEEKDAYS[(now.getDay() + day) % 7]);

/** One line of advice, worked out from the hours it covers. */
function advise(day, now) {
  const hours = hoursFor(day, now);
  const prefix = day === 0 ? '' : `${dayName(day, now)} · `;
  const first = hours.findIndex((x) => x.rain >= 50);
  if (first >= 0) {
    let last = first;
    while (last + 1 < hours.length && hours[last + 1].rain >= 50) last += 1;
    const from = `${pad(hours[first].hour)}:00`;
    const until = `${pad((hours[last].hour + 1) % 24)}:00`;
    const end = last === hours.length - 1;
    const when = first === 0 && end ? (day === 0 ? 'rain for the next 12 hours' : 'rain all day')
      : first === 0 ? `${day === 0 ? 'raining' : 'rain'} until ${until}`
        : end ? `rain from ${from}` : `rain ${from}–${until}`;
    return { icon: 'umbrella', text: `${prefix}${when} · take an umbrella` };
  }
  const warm = hours.reduce((a, b) => (b.temp > a.temp ? b : a));
  if (warm.temp >= 23) return { icon: 'sunny', text: `${prefix}dry and warm · ${warm.temp}° at ${pad(warm.hour)}:00` };
  const cold = day === 0 ? hours.reduce((a, b) => (b.temp < a.temp ? b : a)) : { temp: WEEK[day].lo, hour: 5 };
  if (cold.temp <= 10) {
    return { icon: 'thermostat', text: day === 0 ? `dry · down to ${cold.temp}° by ${pad(cold.hour)}:00` : `${prefix}dry · ${cold.temp}° early, bring a jacket` };
  }
  return { icon: 'check', text: day === 0 ? 'dry for the next 12 hours' : `${prefix}dry all day · no umbrella needed` };
}

export function createWeather(ctx) {
  const { screen, settings } = ctx;
  const root = screen.querySelector('.app[data-app="weather"]');
  const q = (s) => root.querySelector(s);
  const sub = q('#wx-sub');
  const hero = q('.wx-hero');
  const week = q('.wx-week');
  const stripHead = q('.wx-strip-head');
  const stripText = q('#wx-strip-text');
  const hoursEl = q('#wx-hours');
  const advice = q('#wx-advice');
  const cols = [...root.querySelectorAll('.wx-hour')].map((el) => ({
    el, temp: el.querySelector('.wx-hour-temp'), bar: el.querySelector('.wx-bar'), rain: el.querySelector('.wx-hour-rain'), time: el.querySelector('.wx-hour-time'), shown: 0,
  }));
  const rows = [...root.querySelectorAll('.wx-day')].map((el) => ({
    el, day: Number(el.dataset.day), name: el.querySelector('.wx-day-name'), icon: el.querySelector('.wx-day-icon'), rain: el.querySelector('.wx-day-rain'),
    lo: el.querySelector('.wx-day-lo'), hi: el.querySelector('.wx-day-hi'), bar: el.querySelector('.wx-range-bar'), now: el.querySelector('.wx-range-now'),
  }));
  // Switching days, each rain bar springs from its old height to its new one.
  const morph = new Motion(1, { spring: SPRINGS.release, epsilon: 0.002 });
  let from = cols.map(() => 0);
  let to = cols.map(() => 0);
  let drawn = -1;
  let selected = 0;
  let dateKey = -1;
  let updatedKey = '';
  let hourKey = -1;

  const onRange = (t) => `${(((t - WEEK_LO) / (WEEK_HI - WEEK_LO)) * 100).toFixed(2)}%`;

  function fillStatic(now) {
    q('#wx-now-temp').textContent = `${NOW.temp}°`;
    const icon = q('#wx-now-icon');
    icon.textContent = NOW.icon;
    icon.classList.toggle('is-sun', SUNNY.has(NOW.icon));
    q('#wx-now-cond').textContent = NOW.text;
    q('#wx-now-range').textContent = `h ${WEEK[0].hi}° · l ${WEEK[0].lo}°`;
    for (const r of rows) {
      const d = WEEK[r.day];
      const rain = rainOf(r.day);
      r.name.textContent = r.day === 0 ? 'today' : WEEKDAYS[(now.getDay() + r.day) % 7].slice(0, 3);
      r.icon.textContent = d.icon;
      r.rain.textContent = rain >= 20 ? `${rain}%` : '';
      r.lo.textContent = `${d.lo}°`;
      r.hi.textContent = `${d.hi}°`;
      r.bar.style.left = onRange(d.lo);
      r.bar.style.width = `${(((d.hi - d.lo) / (WEEK_HI - WEEK_LO)) * 100).toFixed(2)}%`;
      r.now.style.left = onRange(NOW.temp);
      r.el.classList.toggle('has-now', r.day === 0);
      r.el.setAttribute('aria-label', `${dayName(r.day, now)}: ${d.text}, ${d.lo}° to ${d.hi}°${rain >= 20 ? `, ${rain}% chance of rain` : ''}`);
    }
    ctx.dirty(hero);
    ctx.dirty(week);
  }

  function drawBars() {
    const k = morph.value;
    if (k === drawn) return;
    drawn = k;
    cols.forEach((c, i) => {
      c.shown = clamp(from[i] + (to[i] - from[i]) * k, 0, 100);
      c.bar.style.height = `${c.shown.toFixed(1)}%`;
    });
    ctx.dirty(hoursEl);
  }

  // The strip shows the chosen day's hours; its words change at once and its bars spring to their new heights.
  function showDay(day, animate) {
    const now = new Date();
    const hours = hoursFor(day, now);
    cols.forEach((c, k) => {
      const x = hours[k];
      c.temp.textContent = `${x.temp}°`;
      c.rain.textContent = x.rain >= 20 ? `${x.rain}%` : '';
      c.time.textContent = x.now ? 'now' : pad(x.hour);
      c.el.classList.toggle('is-now', x.now);
    });
    from = cols.map((c) => c.shown);
    to = hours.map((x) => x.rain);
    morph.set(animate && !settings.calm ? 0 : 1);
    if (animate && !settings.calm) morph.to(1, { spring: SPRINGS.release });
    drawn = -1;
    stripText.textContent = day === 0 ? 'next 12 hours' : `${dayName(day, now)} ${dateOf(dayNumber(now) + day).getDate()} · 08–19`;
    const a = advise(day, now);
    q('#wx-advice-icon').textContent = a.icon;
    q('#wx-advice-text').textContent = a.text;
    ctx.dirty(stripHead);
    ctx.dirty(advice);
    drawBars();
  }

  function select(day, animate) {
    selected = day;
    for (const r of rows) {
      const on = r.day === day;
      if (r.el.classList.contains('is-on') === on) continue;
      r.el.classList.toggle('is-on', on);
      r.el.setAttribute('aria-pressed', String(on));
      ctx.recolor(r.el);
    }
    showDay(day, animate);
  }
  for (const r of rows) {
    pressSurface(ctx, r.el, 0.9);
    r.el.addEventListener('click', () => { if (r.day !== selected) select(r.day, true); });
  }

  // Minutes and hours roll on while the app is open: "updated" every ten minutes, today's hours every hour.
  function refresh(force) {
    const now = new Date();
    const date = dayNumber(now);
    if (force || date !== dateKey) { dateKey = date; fillStatic(now); }
    const ten = Math.floor(now.getMinutes() / 10) * 10;
    const updated = `${pad(now.getHours())}:${pad(ten)}`;
    if (force || updated !== updatedKey) { updatedKey = updated; sub.textContent = `home · updated ${updated}`; ctx.dirty(sub); }
    if (force || now.getHours() !== hourKey) { hourKey = now.getHours(); if (force || selected === 0) select(selected, false); }
  }
  refresh(true);

  return {
    beforeOpen() { selected = 0; refresh(true); },
    frame() { refresh(false); drawBars(); return {}; },
  };
}

// ---------- calendar ----------
// Recurring plans by weekday (0 sunday … 6 saturday): [start, minutes, title, place, calendar].
// Calendars carry colours: home amber, family ice, health leaf, work a quiet dim.
const WEEKLY = {
  0: [['11:00', 90, 'brunch', 'at home', 'family']],
  1: [['07:30', 45, 'swim', 'city pool', 'health'], ['09:30', 15, 'standup', 'video call', 'work']],
  2: [['09:30', 15, 'standup', 'video call', 'work'], ['17:30', 90, 'football · leo', 'north park', 'family']],
  3: [['09:30', 15, 'standup', 'video call', 'work'], ['12:30', 60, 'lunch with sam', 'harbour café', 'work']],
  4: [['07:30', 45, 'swim', 'city pool', 'health'], ['09:30', 15, 'standup', 'video call', 'work'], ['18:00', 45, 'piano · ella', 'music school', 'family']],
  5: [['09:30', 15, 'standup', 'video call', 'work'], ['16:00', 45, 'groceries', 'market hall', 'home']],
  6: [['10:00', 60, 'farmers market', 'old square', 'home']],
};
// One-off plans by days from the day the panel started. Tomorrow is exactly what the today page lists.
const PLANS = {
  '-2': { add: [['18:30', 45, 'call grandma', 'video call', 'family']] },
  0: { add: [['19:00', 30, 'bins out', 'recycling', 'home']] },
  1: { only: [['08:15', 20, 'school run', 'oak lane', 'family'], ['11:00', 90, 'plumber', 'kitchen', 'home'], ['19:30', 150, "dinner at mia's", '12 elm street', 'family']] },
  3: { add: [['15:00', 45, 'dentist', 'smile clinic', 'health']] },
  6: { add: [['20:00', 120, 'cinema', 'the rex', 'family']] },
  9: { add: [['14:00', 60, 'car service', 'north garage', 'home']] },
};
const minutesOf = (s) => Number(s.slice(0, 2)) * 60 + Number(s.slice(3));
const lengthText = (m) => (m < 60 ? `${m} min` : `${Math.floor(m / 60)} h${m % 60 ? ` ${pad(m % 60)}` : ''}`);
const untilText = (min) => (min < 60 ? `in ${min} min`
  : min < 180 ? `in ${Math.floor(min / 60)} h ${pad(min % 60)}`
    : min < 1440 ? `in ${Math.round(min / 60)} h` : `in ${plural(Math.round(min / 1440), 'day')}`);
const weekStartOf = (n) => n - ((dateOf(n).getDay() + 6) % 7);

export function createCalendar(ctx) {
  const { screen } = ctx;
  const root = screen.querySelector('.app[data-app="calendar"]');
  const q = (s) => root.querySelector(s);
  const strip = q('#cal-strip');
  const track = q('#cal-track');
  const todayButton = q('#cal-today');
  const nextButton = q('#cal-next');
  const origin = dayNumber();

  function eventsOn(n) {
    const plan = PLANS[n - origin];
    const list = plan?.only ?? [...(WEEKLY[dateOf(n).getDay()] ?? []), ...(plan?.add ?? [])];
    return list.map(([start, minutes, title, place, cal]) => ({ day: n, start, at: minutesOf(start), minutes, title, place, cal })).sort((a, b) => a.at - b.at);
  }

  // Three weeks of live day chips, the current one in the middle, so a drag always has a week to reveal.
  const chips = Array.from({ length: 21 }, (_, i) => {
    const el = document.createElement('button');
    el.type = 'button';
    el.className = 'cal-chip';
    el.tabIndex = -1;
    el.dataset.i = String(i);
    const wd = document.createElement('span');
    wd.className = 'cal-wd mono';
    const date = document.createElement('span');
    date.className = 'cal-date num';
    const dots = document.createElement('span');
    dots.className = 'cal-dots';
    el.append(wd, date, dots);
    track.append(el);
    return { el, wd, date, dots, day: 0, count: -1, on: -1 };
  });
  const rows = [...root.querySelectorAll('.cal-row')].map((el) => ({
    el, time: el.querySelector('.cal-time'), title: el.querySelector('.cal-title'), place: el.querySelector('.cal-place'), len: el.querySelector('.cal-len'),
  }));

  const stripS = ctx.surface(strip);
  const pickS = ctx.surface(q('#cal-pick'));
  const ghostS = ctx.surface(q('#cal-ghost'));
  const slide = new Motion(0, { spring: SPRINGS.page, epsilon: 0.1 }); //  the track's offset in px
  const pickCol = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 }); // the capsule's column in this week
  const squeeze = new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 });
  const ghostFade = new Motion(0, { spring: SPRINGS.effect, epsilon: 0.003 });
  const tracker = velocityTracker();
  let today = origin;
  let weekStart = weekStartOf(today);
  let selected = today;
  let ghost = null; // the selection a page change left behind, leaving with its week
  let pressed = null;
  let dragFrom = 0;
  let shownSlide = null;
  let minuteKey = -1;
  let next = null;

  const size = () => stripS.base?.w || 660 * ctx.unit;
  // The capsule sits on its day's chip wherever the track is, clipped to the strip, concentric with it.
  function capsule(col, fade, squash) {
    const b = stripS.base;
    if (!b || b.w < 1 || fade <= 0.003) return none;
    const u = ctx.unit;
    const pitch = b.w / 7;
    const w = pitch - 10 * u;
    const h = b.h - 16 * u;
    const x = b.x + slide.value + col * pitch + 5 * u;
    if (x > b.x + b.w || x + w < b.x) return none;
    const k = 1 - 0.07 * squash;
    return { x: x + (w * (1 - k)) / 2, y: b.y + 8 * u + (h * (1 - k)) / 2, w: w * k, h: h * k, r: 24 * u * k };
  }
  pickS.geometry = () => capsule(pickCol.value, 1, squeeze.value);
  ghostS.geometry = () => (ghost ? capsule(ghost.col, ghostFade.value, 0) : none);
  ghostS.alpha = () => clamp(ghostFade.value, 0, 1);

  function renderWeek() {
    for (const [i, c] of chips.entries()) {
      const n = weekStart - 7 + i;
      const d = dateOf(n);
      const events = eventsOn(n).length;
      c.day = n;
      c.wd.textContent = WEEKDAYS[d.getDay()].slice(0, 3);
      c.date.textContent = String(d.getDate());
      const count = Math.min(3, events);
      if (count !== c.count) {
        c.count = count;
        c.dots.replaceChildren(...Array.from({ length: count }, () => document.createElement('i')));
      }
      c.el.classList.toggle('is-today', n === today);
      c.el.inert = i < 7 || i > 13;
      c.el.setAttribute('aria-label', `${WEEKDAYS[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()]}${n === today ? ', today' : ''}, ${plural(events, 'event')}`);
      c.el.setAttribute('aria-pressed', String(n === selected));
    }
    // ISO weeks: the week belongs to the year and month of its thursday
    const thursday = dateOf(weekStart + 3);
    const jan4 = new Date(thursday.getFullYear(), 0, 4);
    const week1 = dayNumber(jan4) - ((jan4.getDay() + 6) % 7);
    const sub = q('#cal-sub');
    sub.textContent = `${MONTHS[thursday.getMonth()]} ${thursday.getFullYear()} · week ${1 + Math.floor((weekStart - week1) / 7)}`;
    ctx.dirty(sub);
    const first = dateOf(weekStart);
    strip.setAttribute('aria-label', `Week of ${first.getDate()} ${MONTHS[first.getMonth()]}. Swipe for another week; arrow keys change the day.`);
  }

  function renderAgenda() {
    const list = eventsOn(selected);
    rows.forEach((r, k) => {
      const ev = list[k];
      r.el.classList.toggle('is-empty', !ev);
      if (!ev) return;
      r.el.dataset.cal = ev.cal;
      r.time.textContent = ev.start;
      r.title.textContent = ev.title;
      r.place.textContent = ev.place;
      r.len.textContent = lengthText(ev.minutes);
    });
    const d = dateOf(selected);
    const when = selected === today ? 'today · ' : selected === today + 1 ? 'tomorrow · ' : selected === today - 1 ? 'yesterday · ' : '';
    q('#cal-day-title').textContent = `${WEEKDAYS[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()]}`;
    q('#cal-day-count').textContent = `${when}${list.length ? plural(list.length, 'event') : 'free'}`;
    q('#cal-empty').classList.toggle('is-hidden', list.length > 0);
    for (const c of chips) c.el.setAttribute('aria-pressed', String(c.day === selected));
    const home = selected === today && weekStart === weekStartOf(today);
    if (todayButton.disabled !== home) { todayButton.disabled = home; ctx.dirty(todayButton); }
    ctx.dirty(q('#cal-agenda'));
  }

  // Next up: the first plan that hasn't ended, counted down to the minute.
  function renderNext() {
    const now = new Date();
    const n0 = dayNumber(now);
    const minute = now.getHours() * 60 + now.getMinutes();
    next = null;
    for (let n = n0; n < n0 + 14 && !next; n++) next = eventsOn(n).find((ev) => n > n0 || ev.at + ev.minutes > minute) ?? null;
    const whenEl = q('#cal-next-when');
    const titleEl = q('#cal-next-title');
    const inEl = q('#cal-next-in');
    if (!next) {
      whenEl.textContent = 'next up';
      titleEl.textContent = 'a clear fortnight';
      inEl.textContent = '';
    } else {
      const start = dateOf(next.day);
      start.setHours(Math.floor(next.at / 60), next.at % 60, 0, 0);
      const both = `${next.title} · ${next.place}`;
      titleEl.textContent = both.length <= 24 ? both : next.title;
      if (start <= now) {
        whenEl.textContent = `now · until ${hm(new Date(start.getTime() + next.minutes * 60000))}`;
        inEl.textContent = 'now';
      } else {
        const day = next.day === n0 ? 'today' : next.day === n0 + 1 ? 'tomorrow' : WEEKDAYS[dateOf(next.day).getDay()];
        whenEl.textContent = `next up · ${day} ${next.start}`;
        inEl.textContent = untilText(Math.ceil((start - now) / 60000));
      }
    }
    nextButton.setAttribute('aria-label', `${whenEl.textContent}: ${titleEl.textContent} ${inEl.textContent}`.trim());
    ctx.dirty(nextButton);
  }

  // A page change swaps the weeks under the track and keeps what is on screen exactly where it was; the
  // page spring then carries the new week in with the finger's velocity. The old selection leaves with its week.
  function page(ws, n, direction, velocity, animate) {
    const oldCol = selected - weekStart;
    if (animate) {
      ghost = { col: oldCol - 7 * direction };
      ghostFade.set(1);
      ghostFade.to(0, { spring: SPRINGS.page });
      slide.set(slide.value + direction * size(), velocity);
      slide.to(0, { spring: SPRINGS.page, velocity });
    } else {
      ghost = null;
      slide.set(0);
    }
    weekStart = ws;
    selected = n;
    if (animate && Math.abs(pickCol.target - (n - ws)) < 1e-6) { /* the capsule keeps its column and its motion */ } else pickCol.set(n - ws);
    renderWeek();
    renderAgenda();
  }
  function select(n, animate) {
    const ws = weekStartOf(n);
    if (ws !== weekStart) { page(ws, n, Math.sign(ws - weekStart), 0, animate); return; }
    selected = n;
    if (animate) pickCol.to(n - ws, { spring: SPRINGS.release });
    else pickCol.set(n - ws);
    renderAgenda();
  }

  // Swipe the strip: it tracks 1:1, resists past a whole week, and pages one week per flick (paging pattern).
  ctx.addDragTarget(strip, {
    begin() {
      dragFrom = slide.value;
      slide.set(slide.value);
      tracker.reset();
      pressed = null;
      pickCol.to(selected - weekStart, { spring: SPRINGS.release });
      squeeze.to(0, { spring: SPRINGS.release });
    },
    move(dx, t) {
      const s = size();
      const raw = dragFrom + dx;
      const shown = Math.abs(raw) > s ? Math.sign(raw) * (s + rubberBand(Math.abs(raw) - s, s)) : raw;
      tracker.add(t, shown);
      slide.set(shown, tracker.velocity(t));
    },
    end(t) {
      const v = tracker.velocity(t);
      const direction = clamp(Math.round(-(slide.value + project(v)) / size()), -1, 1);
      if (direction) page(weekStart + 7 * direction, selected + 7 * direction, direction, v, true);
      else slide.to(0, { spring: SPRINGS.page, velocity: v });
    },
  }, 'x');

  // A chip answers on pointer-down: a state layer on the strip from the finger, and the capsule leans toward the
  // day (or squeezes, on the day it already holds). A tap then sends it the whole way.
  track.addEventListener('pointerdown', (e) => {
    const chip = e.target.closest('.cal-chip');
    if (!chip || e.button > 0) return;
    [stripS.lx, stripS.ly] = ctx.local(e);
    stripS.layer.to(0.12, { spring: SPRINGS.layer });
    const col = Number(chip.dataset.i) - 7;
    const current = selected - weekStart;
    pressed = col;
    if (col === current) squeeze.to(1, { spring: SPRINGS.hold });
    else pickCol.to(current + (col - current) * 0.12, { spring: SPRINGS.hold });
  });
  const lift = () => {
    if (pressed === null) return;
    pressed = null;
    stripS.layer.to(0, { spring: SPRINGS.effect });
    squeeze.to(0, { spring: SPRINGS.release });
    pickCol.to(selected - weekStart, { spring: SPRINGS.release });
  };
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) track.addEventListener(type, lift);
  track.addEventListener('click', (e) => {
    const chip = e.target.closest('.cal-chip');
    if (chip) select(weekStart + Number(chip.dataset.i) - 7, true);
  });
  // Keyboard changes land at once.
  strip.addEventListener('keydown', (e) => {
    const step = { ArrowLeft: -1, ArrowRight: 1, PageUp: -7, PageDown: 7 }[e.key];
    if (e.key === 'Home') { e.preventDefault(); select(today, false); return; }
    if (step === undefined) return;
    e.preventDefault();
    select(selected + step, false);
  });

  pressSurface(ctx, todayButton, 0.7);
  todayButton.addEventListener('click', () => select(today, true));
  pressSurface(ctx, nextButton, 0.92);
  nextButton.addEventListener('click', () => { if (next) select(next.day, true); });

  // Live chips draw above the canvas, so they step back while the control center's glass is over them.
  const cc = screen.querySelector('#cc');
  const veil = new Motion(0, { spring: SPRINGS.layer, epsilon: 0.002 });
  let veiled = false;

  // A chip's ink follows how much of the capsule is under it, so words turn as the capsule arrives.
  function ink() {
    const covered = !!cc && !cc.hidden;
    if (covered !== veiled) { veiled = covered; veil.to(covered ? 1 : 0, { spring: SPRINGS.layer }); }
    const opacity = veil.value > 0.001 ? (1 - clamp(veil.value, 0, 1)).toFixed(3) : '';
    if (track.style.opacity !== opacity) track.style.opacity = opacity;
    if (slide.value !== shownSlide) {
      shownSlide = slide.value;
      track.style.transform = `translate3d(${slide.value.toFixed(2)}px, 0, 0)`;
    }
    const fade = ghost ? clamp(ghostFade.value, 0, 1) : 0;
    for (const [i, c] of chips.entries()) {
      const col = i - 7;
      let on = clamp(1 - Math.abs(col - pickCol.value), 0, 1);
      if (ghost) on = Math.max(on, clamp(1 - Math.abs(col - ghost.col), 0, 1) * fade);
      const v = Math.round(on * 100) / 100;
      if (v !== c.on) { c.on = v; c.el.style.setProperty('--on', String(v)); }
    }
    if (ghost && !ghostFade.moving && ghostFade.value < 0.004) ghost = null;
  }

  renderWeek();
  renderAgenda();
  renderNext();

  return {
    // Opens on today, with everything current before the window paints.
    beforeOpen() {
      today = dayNumber();
      weekStart = weekStartOf(today);
      selected = today;
      ghost = null;
      slide.set(0);
      pickCol.set(today - weekStart);
      squeeze.set(0);
      renderWeek();
      renderAgenda();
      minuteKey = Math.floor(Date.now() / 60000);
      renderNext();
      ink();
    },
    frame() {
      const minute = Math.floor(Date.now() / 60000);
      if (minute !== minuteKey) {
        minuteKey = minute;
        const d = dayNumber();
        if (d !== today) { today = d; renderWeek(); renderAgenda(); }
        renderNext();
      }
      ink();
      return {};
    },
  };
}

// ---------- appliances ----------
// Example cycles run twenty times faster than the clock, so the washer finishes about two minutes after the panel
// starts. Minutes left are cycle minutes; "done at" is when the cycle would finish at its real pace.
const SPEED = 20;
const PHASES = {
  washer: [[0.55, 'wash'], [0.82, 'rinse'], [1, 'spin']],
  dryer: [[0.88, 'drying'], [1, 'cooling']],
  dishwasher: [[0.6, 'wash'], [0.78, 'rinse'], [1, 'dry']],
};
const offPeakNow = (d = new Date()) => d.getHours() >= 23 || d.getHours() < 6;
const cycleText = (min) => { const m = Math.max(0, Math.ceil(min)); return m < 60 ? `${m} min` : `${Math.floor(m / 60)} h ${pad(m % 60)}`; };

export function createAppliances(ctx) {
  const { screen } = ctx;
  const root = screen.querySelector('.app[data-app="appliances"]');
  const q = (s) => root.querySelector(s);
  const sub = q('#ap-sub');
  const tipLine = q('#ap-tip-line');
  const sw = q('#ap-delay');
  const swWrap = sw.closest('.ap-switch-wrap');
  const swS = ctx.surface(sw);
  const thumbEl = q('#ap-thumb');
  const thumbS = ctx.surface(thumbEl);
  const thumbPos = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 });
  const thumbPress = new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 });
  const tracker = velocityTracker();
  let delay = false;
  let open = false;
  let dragging = false;
  let dragFrom = 0;
  let shownSub = '';
  let shownTip = '';

  const leftOf = (a, now = Date.now()) => (a.state === 'running' ? Math.max(0, ((a.ends - now) * SPEED) / 60000) : a.left);
  const progress = (a, now) => (a.state === 'done' ? 1 : a.state === 'running' || a.state === 'paused' ? clamp(1 - leftOf(a, now) / a.total, 0, 1) : 0);
  function resume(a, now = Date.now()) {
    a.state = 'running';
    a.ends = now + (a.left * 60000) / SPEED;
    a.doneAt = now + a.left * 60000;
  }
  function start(a) {
    a.seen = false;
    a.left = a.total;
    resume(a);
  }

  const list = [
    { id: 'washer', name: 'washer', icon: 'local_laundry_service', program: 'cotton 40°', total: 118, left: 38, state: 'paused' },
    { id: 'dryer', name: 'dryer', icon: 'heat', program: 'cotton dry', total: 70, left: 70, state: 'idle' },
    { id: 'dishwasher', name: 'dishwasher', icon: 'dishwasher_gen', program: 'eco 50°', total: 170, left: 170, state: 'idle' },
  ].map((a) => {
    const el = q(`#ap-${a.id}`);
    return {
      ...a, el, fillEl: el.querySelector('.ap-fill'), button: el.querySelector('.ap-toggle'), programEl: el.querySelector('.ap-program'),
      leftEl: el.querySelector('.ap-left'), whenEl: el.querySelector('.ap-when'), ends: 0, doneAt: 0, finished: 0, startAt: 0, shown: '',
      fill: new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 }),
    };
  });
  const byId = Object.fromEntries(list.map((a) => [a.id, a]));
  resume(byId.washer); // the washer is on its last 38 minutes when the panel starts
  for (const a of list) a.fill.set(progress(a));

  function sync() {
    const now = Date.now();
    for (const a of list) {
      const active = a.state === 'running' || a.state === 'paused';
      const left = leftOf(a, now);
      const phase = active ? PHASES[a.id].find(([until]) => 1 - left / a.total <= until)?.[1] : null;
      const program = phase ? `${a.program} · ${phase}` : a.program;
      const leftText = a.state === 'done' ? 'done' : cycleText(active ? left : a.total);
      const when = a.state === 'running' ? `done at ${hm(new Date(a.doneAt))}`
        : a.state === 'paused' ? 'paused'
          : a.state === 'scheduled' ? `starts ${hm(new Date(a.startAt))} · done ${hm(new Date(a.startAt + a.total * 60000))}`
            : a.state === 'done' ? `finished ${hm(new Date(a.finished))}` : 'ready';
      const key = `${a.state}|${program}|${leftText}|${when}`;
      if (key === a.shown) continue;
      a.shown = key;
      const wasDone = a.el.classList.contains('is-done');
      a.programEl.textContent = program;
      a.leftEl.textContent = leftText;
      a.whenEl.textContent = when;
      for (const s of ['running', 'paused', 'done']) a.el.classList.toggle(`is-${s}`, a.state === s);
      a.el.classList.toggle('is-idle', a.state === 'idle' || a.state === 'scheduled');
      a.button.querySelector('.icon').textContent = a.state === 'running' ? 'pause' : 'play_arrow';
      a.button.setAttribute('aria-label', `${a.state === 'running' ? 'Pause' : a.state === 'paused' ? 'Resume' : a.state === 'scheduled' ? 'Start now' : 'Start'} the ${a.name}`);
      if (wasDone !== (a.state === 'done')) ctx.recolor(a.fillEl);
      if (open) ctx.dirty(a.el);
    }

    const dish = byId.dishwasher;
    const running = list.filter((a) => a.state === 'running').sort((x, y) => x.ends - y.ends);
    const paused = list.find((a) => a.state === 'paused');
    const done = list.filter((a) => a.state === 'done').sort((x, y) => y.finished - x.finished)[0];
    const subText = running.length ? `${running.length} running · ${running[0].name} done at ${hm(new Date(running[0].doneAt))}`
      : paused ? `${paused.name} paused · ${cycleText(paused.left)} left`
        : dish.state === 'scheduled' ? `dishwasher starts at ${hm(new Date(dish.startAt))}`
          : done ? `${done.name} finished at ${hm(new Date(done.finished))}` : 'all ready';
    if (subText !== shownSub) { shownSub = subText; sub.textContent = subText; if (open) ctx.dirty(sub); }

    // The tip follows the dishwasher and the tariff; the switch rests while it runs, or while power is cheap anyway.
    const offPeak = offPeakNow();
    const busy = dish.state === 'running' || dish.state === 'paused';
    const tip = busy ? (offPeak ? 'the dishwasher is running on off-peak power' : 'the dishwasher is on peak power · delay it next time')
      : dish.state === 'scheduled' ? 'the dishwasher waits for off-peak power · 38% cheaper'
        : offPeak ? 'off-peak power until 06:00 · a good time for the dishwasher' : 'power is 38% cheaper from 23:00 · delay the dishwasher';
    if (tip !== shownTip) { shownTip = tip; tipLine.textContent = tip; if (open) ctx.dirty(tipLine); }
    const disabled = busy || (offPeak && dish.state !== 'scheduled');
    if (sw.disabled !== disabled) {
      sw.disabled = disabled;
      swWrap.classList.toggle('is-off', disabled);
      if (open) ctx.dirty(swWrap);
    }
  }

  function setDelay(on, velocity = 0, announce = true) {
    const dish = byId.dishwasher;
    const want = on && dish.state !== 'running' && dish.state !== 'paused' && !(offPeakNow() && dish.state !== 'scheduled');
    thumbPos.to(want ? 1 : 0, { spring: SPRINGS.release, velocity });
    if (want !== delay) {
      delay = want;
      sw.setAttribute('aria-checked', String(want));
      ctx.recolor(sw);
      ctx.recolor(thumbEl);
      if (want) {
        const at = new Date();
        at.setHours(23, 0, 0, 0);
        dish.state = 'scheduled';
        dish.startAt = at.getTime();
        if (announce) ctx.toast(`dishwasher starts at ${hm(at)}`, 'schedule');
      } else if (dish.state === 'scheduled') {
        dish.state = 'idle';
        if (announce) ctx.toast('delay off · dishwasher ready', 'dishwasher_gen');
      }
    }
    sync();
  }

  function press(a) {
    const now = Date.now();
    if (a.state === 'running') {
      a.left = leftOf(a, now);
      a.state = 'paused';
    } else if (a.state === 'paused') {
      resume(a, now);
    } else {
      const scheduled = a.state === 'scheduled';
      start(a);
      if (scheduled) {
        setDelay(false, 0, false);
        ctx.toast('dishwasher started now · delay off', a.icon);
      }
    }
    sync();
  }

  // Cycles keep running while the app is closed; the island says when one is done, and a delayed start begins on time.
  function tick() {
    const now = Date.now();
    for (const a of list) {
      if (a.state === 'running' && leftOf(a, now) <= 0) {
        a.state = 'done';
        a.left = 0;
        a.finished = now;
        const dryer = byId.dryer;
        ctx.toast(a.id === 'washer' && (dryer.state === 'idle' || dryer.state === 'done') ? 'washer is done · move the laundry to the dryer' : `${a.name} is done · ${a.program}`, a.icon);
        sync();
      } else if (a.state === 'scheduled' && now >= a.startAt) {
        start(a);
        setDelay(false, 0, false);
        ctx.toast('dishwasher started · off-peak power', a.icon);
      }
    }
  }

  for (const a of list) {
    const fill = ctx.surface(a.fillEl);
    fill.geometry = () => {
      const c = fill.clip;
      const p = clamp(a.fill.value, 0, 1);
      return !c || p <= 0.001 ? none : { x: c.x, y: c.y, w: c.w * p, h: c.h, r: 0 };
    };
    pressSurface(ctx, a.button, 0.6);
    a.button.addEventListener('click', () => press(a));
  }

  // The switch: a press grows the thumb at once; a tap flips it; a drag carries the thumb 1:1 and lets go with
  // the finger's velocity, landing on whichever side the flick projects to.
  thumbS.geometry = () => {
    const b = swS.base;
    if (!b || b.w < 1) return none;
    const u = ctx.unit;
    const d = (34 + 12 * clamp(thumbPos.value, 0, 1) + 8 * thumbPress.value) * u;
    const cx = b.x + b.h / 2 + clamp(thumbPos.value, -0.1, 1.1) * (b.w - b.h);
    const cy = b.y + b.h / 2;
    return { x: cx - d / 2, y: cy - d / 2, w: d, h: d, r: d / 2 };
  };
  pressSurface(ctx, sw, 1);
  sw.addEventListener('pointerdown', (e) => { if (e.button <= 0 && !sw.disabled) thumbPress.to(1, { spring: SPRINGS.hold }); });
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) sw.addEventListener(type, () => { if (!dragging) thumbPress.to(0, { spring: SPRINGS.release }); });
  sw.addEventListener('click', () => setDelay(!delay));
  q('#ap-delay-label').addEventListener('click', () => { if (!sw.disabled) setDelay(!delay); });
  ctx.addDragTarget(sw, {
    begin() {
      dragging = !sw.disabled;
      dragFrom = thumbPos.value;
      tracker.reset();
      if (dragging) thumbPress.to(1, { spring: SPRINGS.hold });
    },
    move(dx, t) {
      if (!dragging) return;
      const b = swS.base;
      const raw = dragFrom + dx / Math.max(1, b.w - b.h);
      const value = raw < 0 ? -rubberBand(-raw, 1) * 0.15 : raw > 1 ? 1 + rubberBand(raw - 1, 1) * 0.15 : raw;
      tracker.add(t, value);
      thumbPos.set(value, tracker.velocity(t));
    },
    end(t) {
      if (!dragging) return;
      dragging = false;
      const v = tracker.velocity(t);
      thumbPress.to(0, { spring: SPRINGS.release });
      setDelay(thumbPos.value + project(v, 0.99) > 0.5, v);
    },
  }, 'x');

  // What the live activities show: cycles running or paused, and a finished one until the app is opened.
  ctx.appliancesList = () => {
    const now = Date.now();
    return list
      .filter((a) => a.state === 'running' || a.state === 'paused' || (a.state === 'done' && !a.seen && now - a.finished < 10 * 60000))
      .map((a) => ({
        label: a.name, icon: a.icon, done: a.state === 'done', paused: a.state === 'paused', progress: progress(a, now),
        left: (leftOf(a, now) * 60) / SPEED, value: a.state === 'done' ? 'done' : cycleText(leftOf(a, now)),
      }));
  };

  ctx.onFrame(() => {
    tick();
    const now = Date.now();
    for (const a of list) {
      const p = progress(a, now);
      if (Math.abs(p - a.fill.target) > 0.02) a.fill.to(p, { spring: SPRINGS.release });
      else if (!a.fill.moving && a.fill.value !== p) a.fill.set(p);
    }
    if (open) sync();
    return {};
  });
  sync();

  return {
    beforeOpen() {
      open = true;
      for (const a of list) { a.shown = ''; if (a.state === 'done') a.seen = true; }
      shownSub = '';
      shownTip = '';
      sync();
    },
    open() { open = true; },
    close() { open = false; },
  };
}

// ---------- plants ----------
// Moisture falls from full to dry over each plant's interval; at DRY it wants water today. Example data.
const DRY = 0.2;
const PLANTS = [
  { id: 'monstera', name: 'monstera', every: 7, moisture: 0.53 },
  { id: 'basil', name: 'basil', every: 2, moisture: 0.14 },
  { id: 'orchid', name: 'orchid', every: 9, moisture: 0.47 },
  { id: 'snake', name: 'snake plant', every: 14, moisture: 0.81 },
];
const dueText = (days) => (days === 0 ? 'water today' : days === 1 ? 'water tomorrow' : `water in ${days} days`);

export function createPlants(ctx) {
  const { screen } = ctx;
  const root = screen.querySelector('.app[data-app="plants"]');
  const sub = root.querySelector('#pl-sub');
  const plants = PLANTS.map((p) => {
    const el = root.querySelector(`#pl-${p.id}`);
    return {
      ...p, el, fillEl: el.querySelector('.pl-fill'), moistEl: el.querySelector('.pl-moist'), dueEl: el.querySelector('.pl-due'), button: el.querySelector('.pl-water'),
      level: new Motion(p.moisture, { spring: SPRINGS.release, epsilon: 0.001 }), watered: 0, pct: -1, shown: '',
    };
  });
  let open = false;
  let shownSub = '';

  const wateredToday = (p) => p.watered > 0 && dayNumber(new Date(p.watered)) === dayNumber();
  // Days until dry, from where the moisture is heading, so the words change the moment a plant is watered.
  const daysLeft = (p) => (p.level.target <= DRY + 1e-6 ? 0 : Math.ceil((p.level.target - DRY) / ((1 - DRY) / p.every) - 1e-6));
  const isDue = (p) => daysLeft(p) === 0 && !wateredToday(p);

  function sync() {
    for (const p of plants) {
      const days = daysLeft(p);
      const done = wateredToday(p);
      const due = days === 0 && !done;
      const key = `${days}|${done}`;
      if (key === p.shown) continue;
      p.shown = key;
      const wasDue = p.button.classList.contains('is-due');
      p.dueEl.textContent = dueText(days);
      p.el.classList.toggle('is-due', due);
      p.button.classList.toggle('is-due', due);
      p.button.classList.toggle('is-done', done);
      p.button.querySelector('.icon').textContent = done ? 'check' : 'water_drop';
      p.button.querySelector('.mono').textContent = done ? `watered ${hm(new Date(p.watered))}` : 'watered';
      p.button.setAttribute('aria-label', done ? `${p.name} was watered at ${hm(new Date(p.watered))}` : `Mark the ${p.name} as watered`);
      if (wasDue !== due) ctx.recolor(p.button);
      if (open) ctx.dirty(p.el);
    }
    const due = plants.filter(isDue);
    const soonest = plants.slice().sort((a, b) => daysLeft(a) - daysLeft(b))[0];
    const text = due.length === 1 ? `${due[0].name} needs water today`
      : due.length ? `${due.length} plants need water today`
        : `nothing due today · next is ${soonest.name}, ${dueText(daysLeft(soonest)).replace('water ', '')}`;
    if (text !== shownSub) { shownSub = text; sub.textContent = text; if (open) ctx.dirty(sub); }
  }

  // The percentage counts along with the water as it springs up.
  function moisture() {
    for (const p of plants) {
      const pct = Math.round(clamp(p.level.value, 0, 1) * 100);
      if (pct === p.pct) continue;
      p.pct = pct;
      p.moistEl.textContent = `${pct}%`;
      if (open) ctx.dirty(p.moistEl);
    }
  }

  for (const p of plants) {
    const fill = ctx.surface(p.fillEl);
    // Water rises from the bottom and runs past it, so the card's own clip draws the fill's corners.
    fill.geometry = () => {
      const c = fill.clip;
      const h = c ? clamp(p.level.value, 0, 1.04) * c.h : 0;
      return h < 0.5 ? none : { x: c.x, y: c.y + c.h - h, w: c.w, h: h + 40 * ctx.unit, r: 0 };
    };
    pressSurface(ctx, p.button, 0.7);
    p.button.addEventListener('click', () => {
      if (wateredToday(p)) { ctx.toast(`${p.name} was watered at ${hm(new Date(p.watered))}`, 'water_drop'); return; }
      p.watered = Date.now();
      p.level.to(1, { spring: SPRINGS.release });
      ctx.toast(`${p.name} watered · next in ${plural(p.every, 'day')}`, 'water_drop');
      sync();
    });
  }

  /** For the energy tree: plants watered today, and plants still waiting for water today. */
  ctx.plants = {
    careToday: () => plants.filter(wateredToday).length,
    dueToday: () => plants.filter(isDue).length,
  };

  sync();
  moisture();

  return {
    beforeOpen() {
      open = true;
      for (const p of plants) { p.shown = ''; p.pct = -1; }
      shownSub = '';
      sync();
      moisture();
    },
    open() { open = true; },
    close() { open = false; },
    frame() { sync(); moisture(); return {}; },
  };
}
