# How Shadow Fleet evolved

Shadow Fleet started as a single round screen showing a star field. About 280 versions and a lot of playtests later, it is a four-player board game: a 3D-printed galaxy, a table console in the middle, and every player's phone as their private command deck.

This page is the short story of how it got here, and why it looks the way it does.

## At a glance

| When (2026) | Version | What changed |
|---|---|---|
| Before 23 September | v0.1 to v0.18 | One round screen becomes a game: phases, planets, battles, reputation, victory points |
| Late September | v0.19 to v0.21 | Animated battles, a second screen size, a full interface redesign, hologram transmissions |
| October | v0.22 to v0.30 | The round screen moves to the centre of the map and becomes the **table**; phones join over Wi-Fi |
| Late October | 2.0 | Home worlds, 17 worlds, a two-piece printed map, quests, markets |
| November 6 | 3.0 design | Characters and classes, faction favour, Eras, Galaxy News |
| November 6 to 7 | 3.1 build | The whole design built in eight stages, then shaken down by robot players |
| November 8 to 23 | 3.1 polish | Weekly playtests: the Final Hour, the black market, buyouts and uprisings, a guided first game |

## 1. A screen that wanted to be a game (v0.1 to v0.18)

The first version was a Waveshare 1.43" round AMOLED showing a star field and some static screens. Each early version added one real piece of a game:

- **v0.3:** the screen starts its own Wi-Fi, and a phone can join as the defender in a battle.
- **v0.5:** rounds and phases (Invest, Movement, Resolution).
- **v0.6:** taking planets: the first real way to win.
- **v0.7:** real multiplayer. Game state stopped being hard-coded for two players.
- **v0.10 to v0.12:** faction transmissions (a face that speaks when your reputation moves) and the reputation system behind them.
- **v0.13:** victory points. The game finally had an ending.
- **v0.14:** the full galaxy of 12 planets.
- **v0.15 to v0.18:** seats, factions and a lobby, so anyone can sit down and play.

Most of the lessons from this era were about the hardware: a touch screen calibrated by measuring instead of guessing, crashes traced to a task's stack running out, and a screen that flashed green at boot.

## 2. Making it feel good (v0.19 to v0.21, September)

Version control started at v0.19, and with it a habit that stuck: every change gets a version number and a written reason.

- **Animated battles** on both the console and the phone: dice that tumble and land, ships that blink and explode, a pixel planet turning in the background.
- **A second console size.** The 1.75" round screen was brought up from the manufacturer's documentation and worked on the first try.
- **A full interface redesign** in seven steps: the home hub, standings, the galaxy map, planet pages, negotiation, factions, and commander portraits drawn in code (original pixel art, no film assets).
- **Phones got reliable.** The playtest of 12 October found taps going missing. The phone page had been rebuilding itself every 0.7 seconds and swallowing presses mid-tap. It now changes only what changed.

## 3. The table is born (v0.22 to v0.30, October)

The biggest shift of the project: the round screen stopped being a player's device and moved into **the centre of the map**, a pocket in the printed board made for it.

- The table **hosts the game**. Phones join by scanning its QR code, on its own network or the house Wi-Fi.
- A custom **glow renderer** gives the table its look: one mood colour per phase, gates at each player's side that close when they lock in, dice facing each fighter, and big moments that chase around the rim (FIRST BLOOD, THE CORE FALLS).
- **Negotiation became a conversation.** A faction asks three to five questions and judges your answers, instead of handing you a price.
- **Save states:** pull the plug mid-game and the table resumes exactly where it was.
- The **shuffled galaxy**, so every game deals the worlds differently.

## 4. Shadow Fleet 2.0 (late October)

Playtests kept asking for more to do on your own turn and more reasons to care about the map. 2.0 answered with:

- **Home worlds:** each faction starts holding its own.
- **17 worlds** on a bigger map, printed in **two snap-together halves** with the table screen in a hub in the middle.
- Quests, markets with limited stock, and planet traits you can feel in play.
- **Simpler scoring.** Victory points now come only from worlds held, your quest, destroying a fleet, and events. Everything else was cut.

From here on, the phones became the main way to play, and new work landed on the phone and the table first.

## 5. Shadow Fleet 3.0: a design, before any code (6 November)

One long design session rewrote the game around a single idea: **you are a character, and the galaxy remembers what you do.**

- **16 characters**, each with a backstory, an age, a ship and a quirk, in **four classes**: Warlord, Consul, Infiltrator and Scoundrel. Each class has its own strengths, weakness, quest and once-a-game move.
- **Favour** with each of the four factions replaces simple reputation. Climb a faction's ladder for gifts. Reach the top and you become its **Champion**.
- A Champion can declare an **Era**: the Spice Age, Imperial Order, the Rebellion Rises, or the Dawn's grip. It changes one rule for everyone.
- **Galaxy News** at the start of every round: what the factions are doing, and one thing just for you.
- **The golden rule**, written that day and checked against every feature since: *you can feel it.* Every strength, weakness and hidden effect has to show itself when it fires.

Every screen was mocked up and agreed before building started, along with a style guide: a modern frame with a retro soul, pixel icons and numerals, and smooth motion.

## 6. Building 3.1 (6 to 7 November)

The design went in as eight stages over two days:

1. A new phone foundation (smooth screen changes, bottom sheets, notifications, an event log)
2. The characters
3. The favour spine
4. Galaxy News and grudges
5. Champions and Eras
6. Once-a-game moves
7. The galaxy map
8. A finishing pass

Then came the **shake-down.** Robot players (`tools/shakedown/bot.py`) played whole games against the real table over Wi-Fi, with the table restarted mid-game to prove every save resumed. That shook out crashes no human playtest had hit yet.

## 7. Playtest, fix, repeat (8 to 23 November)

From here the game was shaped by the table, one evening at a time:

- **The Final Hour.** The last round is played in secret. Everyone writes Last Orders, then the strikes are revealed one at a time on the table, in step on every phone.
- **The black market:** a smuggler's den with lots for the late game, and a cargo hold to keep them in.
- **Buyouts and uprisings:** new ways to take a world someone else holds.
- **Fairer battles.** Dice were checked and were fair. The forecast was wrong, and a bigger side now wins ties at a planet.
- **A broken table console.** A dropped table screen cracked mid-month. Its replacement is the one in the build guide.
- **The "frozen table" that wasn't frozen.** Some routers reject a device's first login after a restart. The table now retries instead of giving up on home Wi-Fi.
- **Phones that ran hot.** Animations now pause when nobody is touching the screen, and polling slows down when nothing is happening.
- **For new players:** Rivet, a droid who walks you through your first game on the real screens; a pop-up the first time you fight, negotiate or see a promise; and a plain-words guide to who each faction is.

## What carried through

Some choices held from the first version to the last:

- **A companion, not a replacement.** Ships live on the printed map. The software holds what a board can't: hidden information, money, reputation, dice and score.
- **Every device is an equal player.** No player is the host.
- **Reward, don't punish.** Bad choices cost reputation and money, never victory points.
- **Original art only.** Every face, planet and icon is drawn from code.

## By the numbers

- About **280 versions**, each with a written changelog entry
- **537 commits** since version control started on 23 September
- **16 characters**, **17 worlds**, **4 factions**, **4 classes**, **4 Eras**
- Roughly **23,000 lines** of game, phone and table code
- Regular playtests at a real table, plus robot games after every big change
