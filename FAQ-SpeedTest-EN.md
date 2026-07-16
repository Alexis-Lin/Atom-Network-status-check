# FAQ · Network Speed Test (English)

> **In one line**: this is a **pre-class network self-check** — some ATOM class features (such as AI-powered interaction) need a stable connection, and a quick test tells you whether the WiFi (or phone hotspot) you're on can carry a smooth class.
>
> Entry: inside the speed-test app, **long-press any blank area (~0.8s)** to open this guide (interaction spec: [PRD-2 A4.9](./PRD-2-网速测试小程序.md)). 中文版见 [`FAQ-网速测试常见问题.md`](./FAQ-网速测试常见问题.md).
>
> **Copy policy**: consumer-facing, simple and short; **conservative on privacy and data** — no description of what data flows where; up/down are explained only as "sending / receiving speed".

---

## Full version (7 questions + 1 tip)

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

### 6. Should I worry about occasional weak signal?

Not really — networks fluctuate, and **an occasional dip is normal**. If a class hiccups now and then, ATOM **automatically tries to reconnect**; you usually don't need to do anything. That said, we recommend using ATOM **somewhere with reasonably good network speed** — and in a weak-signal environment, **connecting to your phone's 5G hotspot is usually the better choice** (see the guide at the end).

### 7. Do I need to run a test manually before every class?

No. **The system checks your network periodically on its own**; if the connection stays poor for a while, you'll **receive a push notification**. You generally don't need to think about this — it's just good to know it's there, and you can always run a manual test whenever you want to double-check. **If the network stays poor, chances are the environment itself isn't ideal** — consider **switching to a phone hotspot** or **a different WiFi**.

> **Tip**: if a problem won't go away, **screenshot the result page and send it to support** — one image is all they need.

---

## On-screen condensed version (round screen, English)

> Spec: title 28px; question 21px white bold, answer 21px gray (the FAQ is a long-read page and uses one size smaller as an exception; the global 3-tier type scale is unchanged), each answer ≤ 3 lines.

| # | Question | Answer |
| --- | --- | --- |
| 1 | Why this test? | Some class features need a stable connection — this checks yours. |
| 2 | Same as WiFi bars? | No — those show signal strength; these show measured quality. |
| 3 | Vs. phone speed tests? | Those test top speed; this tests "enough for class". |
| 4 | What are Up / Down? | Up = sending speed; Down = receiving speed. Class needs both. |
| 5 | Does it cost data? | A little, only when you tap Test; never during class. |
| 6 | Occasional weak signal? | Networks fluctuate — we auto-reconnect in class. Best used where the network is good. |
| 7 | Test manually every time? | No — we check periodically and push you if it stays poor; that usually means the environment — try a hotspot or another WiFi. |
| Hotspot | Weak-signal environment? | Try your phone's 5G hotspot — see the "How to connect a phone hotspot" link. |
| Tip | Still stuck? | Screenshot the result page for support. |

---

## Appendix · Connecting to a 5G hotspot

When local WiFi is weak, a phone 5G hotspot usually gives a steadier class experience:

1. Turn on your phone's **Personal Hotspot**;
2. In ATOM's WiFi list, select your phone's hotspot and connect;
3. Come back to this app and run one test — a "Good speed" result means you're set.

> **Tips**: a step-by-step guide is linked at the bottom of the FAQ page → **"How to connect a phone hotspot"** (routes to a system help page, configured by the client; that page also covers the iPhone Personal Hotspot "Maximize Compatibility" switch — some devices can only find the hotspot when it's on).

---

## Entry & interaction (summary; spec in PRD-2 A4.9)

- **Trigger**: on any page of the app, long-press a blank area (outside buttons / the report card) for **~0.8s** with light haptic feedback → fullscreen FAQ;
- **Discovery**: a one-time hint on first launch — "Long-press anywhere to learn more";
- **Form**: title "About this test" (28px) + scrollable Q&A (21px bold questions / 21px gray answers); **swipe right to exit**, or tap the solid "OK" button at the bottom — both return to the previous page with state preserved (a running test is not interrupted); content is bundled locally and **readable offline**;
- **One jump link at the bottom of the FAQ page**: "How to connect a phone hotspot" → system help page (which also covers Maximize Compatibility); followed by the "OK" button and a "swipe right anytime to exit" hint;
- Single source of truth for copy = this file (EN) and the CN file; on-screen text follows the condensed table.
