import asyncio
import os
import random
import nats

NATS_URL = os.environ.get("NATS_URL", "nats://127.0.0.1:4222")
HEDGER = os.environ.get("HEDGER_SENDER", "HEDGE001")
QUOTER = os.environ.get("SENDER", "QUOTE001")
TAKER = os.environ.get("TAKER_SENDER", "PYTKR001")

FEEDS = ["AAH6", "AAM6", "AAU6"]
HEDGE_FEED = "AAH6"        # Most liquid feed, used for hedging execution
MAX_HEDGE_CLIP = 10       # Max contracts per single hedge order
HEDGE_THRESHOLD = 15      # Start hedging when |desk_total| >= this
HEDGE_TARGET = 5           # Try to reduce desk position to within +/- this
HEDGE_COOLDOWN_S = 0.2     # Wait between hedge slices to let fills process
STATUS_INTERVAL_S = 5.0    # Print desk status every N seconds


class DeskHedger:
    def __init__(self, nc):
        self.nc = nc
        self.oid = random.randint(0, 80_000_000)

        # Track position per-sender, per-feed for full visibility
        # positions[sender][feed] = int
        self.positions = {
            QUOTER: {f: 0 for f in FEEDS},
            TAKER:  {f: 0 for f in FEEDS},
            HEDGER: {f: 0 for f in FEEDS},
        }

        # BBO cache per feed for hedging execution
        self.bbo = {f: {"bid": None, "bid_vol": 0, "ask": None, "ask_vol": 0} for f in FEEDS}

        self.hedge_fills = 0
        self.lock = asyncio.Lock()

    def next_oid(self):
        self.oid += 1
        return f"{self.oid:08d}"

    def desk_position_by_feed(self, feed):
        """Total position for one feed across all senders."""
        return sum(self.positions[s][feed] for s in self.positions)

    def desk_total(self):
        """Net desk position across ALL feeds and ALL senders."""
        return sum(self.desk_position_by_feed(f) for f in FEEDS)

    def sender_total(self, sender):
        """Total position for one sender across all feeds."""
        return sum(self.positions[sender][f] for f in FEEDS)

    # ---- Market Data Callbacks ----

    async def on_bbo(self, msg):
        f = msg.data.decode().split()
        if len(f) < 6:
            return
        feed = f[1]
        if feed not in self.bbo:
            return
        self.bbo[feed]["bid"] = None if f[2] == "-" else int(f[2])
        self.bbo[feed]["bid_vol"] = 0 if f[2] == "-" else int(f[3])
        self.bbo[feed]["ask"] = None if f[4] == "-" else int(f[4])
        self.bbo[feed]["ask_vol"] = 0 if f[4] == "-" else int(f[5])

    async def on_execution(self, msg, sender_id):
        data = msg.data.decode().split()
        if len(data) < 8 or data[1] != "E":
            return

        # Extract feed from NATS subject: ex.md.<FEED>.<SENDER>
        subj_parts = msg.subject.split(".")
        if len(subj_parts) < 4:
            return
        feed = subj_parts[2]
        if feed not in FEEDS:
            return

        incoming_id = data[2]   # 17-char: SENDER01:00000001
        resting_id = data[3]
        vol = int(data[4])
        aggressor = data[7]     # B or S

        # Figure out if THIS sender bought or sold
        if incoming_id.startswith(sender_id):
            we_bought = (aggressor == "B")
        elif resting_id.startswith(sender_id):
            we_bought = (aggressor == "S")
        else:
            return

        delta = vol if we_bought else -vol

        async with self.lock:
            self.positions[sender_id][feed] += delta

    # ---- Hedging Logic (runs as a periodic loop) ----

    def decide_hedge(self):
        """
        Pure decision step: given current positions and BBO, return
        (feed, side, qty, price, desk_total) for the next hedge order,
        or None if no hedge is needed / possible right now.
        Caller must hold self.lock (reads positions and bbo).
        """
        total = self.desk_total()

        if abs(total) < HEDGE_THRESHOLD:
            return None

        bbo = self.bbo.get(HEDGE_FEED, {})
        bid = bbo.get("bid")
        ask = bbo.get("ask")

        if bid is None or ask is None:
            return None

        if total > 0:
            qty = min(total - HEDGE_TARGET, MAX_HEDGE_CLIP)
            if qty <= 0:
                return None
            return (HEDGE_FEED, "S", qty, bid - 3, total)
        else:
            qty = min(abs(total) - HEDGE_TARGET, MAX_HEDGE_CLIP)
            if qty <= 0:
                return None
            return (HEDGE_FEED, "B", qty, ask + 3, total)

    async def hedge_loop(self):
        """
        Periodically checks the desk's net position and hedges if needed.
        This loop-based approach is more robust than purely reactive hedging,
        because it catches positions that build up between execution callbacks.
        """
        while True:
            await asyncio.sleep(HEDGE_COOLDOWN_S)

            async with self.lock:
                decision = self.decide_hedge()

            if decision is None:
                continue

            hedge_feed, side, qty, price, total = decision

            oid = self.next_oid()
            order = f"{HEDGER} A {hedge_feed} {oid} {side} {qty} {price} F"
            req_subject = f"ex.req.{HEDGER}"

            action = "SELL" if side == "S" else "BUY"
            print(f"[Hedger] HEDGE: Desk={total:+d} → {action} {qty} {hedge_feed} @ {price}", flush=True)

            try:
                reply = await self.nc.request(req_subject, order.encode(), timeout=1.0)
                parts = reply.data.decode().split()
                if len(parts) >= 2 and parts[1] == "Y":
                    filled = int(parts[2]) if len(parts) > 2 else 0
                    self.hedge_fills += 1
                    print(f"[Hedger] Accepted (filled={filled})", flush=True)
                else:
                    print(f"[Hedger] Rejected: {reply.data.decode()}", flush=True)
            except Exception as e:
                print(f"[Hedger] Order failed: {e}", flush=True)

    # ---- Status Reporter ----

    async def status_loop(self):
        """Print a desk summary every few seconds so we can see the big picture."""
        while True:
            await asyncio.sleep(STATUS_INTERVAL_S)
            async with self.lock:
                total = self.desk_total()
                taker_pos = self.sender_total(TAKER)
                quoter_pos = self.sender_total(QUOTER)
                hedger_pos = self.sender_total(HEDGER)

            per_feed = " | ".join(
                f"{f}={self.desk_position_by_feed(f):+d}" for f in FEEDS
            )
            print(
                f"[Hedger] DESK: total={total:+d}  "
                f"(taker={taker_pos:+d} quoter={quoter_pos:+d} hedger={hedger_pos:+d})  "
                f"[{per_feed}]  hedge_fills={self.hedge_fills}",
                flush=True
            )


async def main():
    nc = await nats.connect(NATS_URL)
    hedger = DeskHedger(nc)

    # Subscribe to BBO for all feeds (so we can hedge on any)
    for feed in FEEDS:
        await nc.subscribe(f"ex.bbo.{feed}", cb=hedger.on_bbo)

    # Wiretap executions for all 3 senders across all feeds
    async def cb_quoter(msg): await hedger.on_execution(msg, QUOTER)
    async def cb_taker(msg):  await hedger.on_execution(msg, TAKER)
    async def cb_hedger(msg): await hedger.on_execution(msg, HEDGER)

    await nc.subscribe(f"ex.md.*.{QUOTER}", cb=cb_quoter)
    await nc.subscribe(f"ex.md.*.{TAKER}",  cb=cb_taker)
    await nc.subscribe(f"ex.md.*.{HEDGER}", cb=cb_hedger)

    print(f"[Hedger] Online. threshold={HEDGE_THRESHOLD} target={HEDGE_TARGET} clip={MAX_HEDGE_CLIP}", flush=True)
    print(f"[Hedger] Watching: taker={TAKER} quoter={QUOTER} hedger={HEDGER}", flush=True)

    # Launch the hedge loop and status reporter as background tasks
    asyncio.create_task(hedger.hedge_loop())
    asyncio.create_task(hedger.status_loop())

    try:
        await asyncio.Future()  # Keep alive forever
    finally:
        await nc.drain()


if __name__ == "__main__":
    asyncio.run(main())
