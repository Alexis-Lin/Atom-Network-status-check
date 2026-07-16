# FAQ · Network Speed Test (English)

> **In one line**: this is a **pre-class network self-check** — some ATOM class features (such as AI-powered interaction) need a stable connection, and a quick test tells you whether the WiFi (or phone hotspot) you're on can carry a smooth class.
>
> Entry: inside the speed-test app, **long-press any blank area (~0.8s)** to open this guide (interaction spec: [PRD-2 A4.9](./PRD-2-网速测试小程序.md)). 中文版见 [`FAQ-网速测试常见问题.md`](./FAQ-网速测试常见问题.md).
>
> **Copy policy**: consumer-facing, simple and short; **conservative on privacy and data** — no description of what data flows where; up/down are explained only as "sending / receiving speed".

---

## Full version (5 questions + 1 tip)

### 1. Why does ATOM need its own network test?

Some class features — like AI-powered interaction — need a stable connection. Half a minute of testing tells you whether the **WiFi or phone hotspot** you're on right now can carry a smooth class.

### 2. Are these three bars the same as my WiFi signal bars?

**No.** The WiFi bars on your phone or router show **wireless signal strength**; the three bars here show **measured network quality** — how fast and how stable the connection actually is. **Full signal bars with mediocre network quality is common**, so it's normal for the two to disagree. Trust the measured result here.

### 3. How is this different from speed-test apps on my phone?

Phone speed tests measure "**how fast your connection can go at its peak**"; ATOM measures "**whether it's enough for a class**". The results won't always match — a fast broadband plan can still show a yellow light here, for example when the wireless environment is noisy or many devices are online at once.

### 4. What do "Up" and "Down" mean?

**Up** is how fast your device **sends data**; **Down** is how fast it **receives data**. A smooth interactive class needs both directions — if either falls short, the report highlights it separately so you know what to fix.

### 5. Does testing use data? Will it affect my home network?

It uses a small amount of data, only while you run a test, and stops as soon as the test ends. Testing is unavailable during class, so it never affects a class in progress.

> **Tip**: even after a "Good speed" result, occasional lag can happen — networks fluctuate (for example when several devices are busy at once). During class, ATOM keeps watching the connection and adapts automatically; you don't need to do anything. If something goes wrong, **screenshot the result page and send it to support** — one image is all they need.

---

## On-screen condensed version (round screen, English)

> Spec: question 24px white bold, answer 24px gray, each answer ≤ 3 lines.

| # | Question | Answer |
| --- | --- | --- |
| 1 | Why this test? | Some class features need a stable connection — this checks yours. |
| 2 | Same as WiFi bars? | No — those show signal strength; these show measured quality. |
| 3 | Vs. phone speed tests? | Those test top speed; this tests "enough for class". |
| 4 | What are Up / Down? | Up = sending speed; Down = receiving speed. Class needs both. |
| 5 | Does it cost data? | A little, only when you tap Test; never during class. |
| Tip | Lag in class? | We auto-adapt; screenshot the result for support. |

---

## Entry & interaction (summary; spec in PRD-2 A4.9)

- **Trigger**: on any page of the app, long-press a blank area (outside buttons / the report card) for **~0.8s** with light haptic feedback → fullscreen FAQ;
- **Discovery**: a one-time hint on first launch — "Long-press anywhere to learn more";
- **Form**: title "About this test" (32px) + scrollable Q&A (24px bold questions / 24px gray answers); swipe down or tap blank space to close, returning to the previous page with state preserved (a running test is not interrupted); content is bundled locally and **readable offline**;
- Single source of truth for copy = this file (EN) and the CN file; on-screen text follows the condensed table.
