"""Unit tests for hedger/hedger.py — no network, no exchange, pure logic."""
import asyncio

import pytest

import hedger as hg
from hedger import DeskHedger

QUOTER = hg.QUOTER
TAKER = hg.TAKER
HEDGER = hg.HEDGER


def run(coro):
    return asyncio.run(coro)


class FakeMsg:
    def __init__(self, data, subject=""):
        self.data = data if isinstance(data, bytes) else data.encode()
        self.subject = subject


class FakeReply:
    def __init__(self, data):
        self.data = data if isinstance(data, bytes) else data.encode()


class FakeNC:
    """Records request() calls; returns a canned reply or raises."""

    def __init__(self, reply=b"EXCHANGE Y 10", raise_exc=None):
        self.reply = reply
        self.raise_exc = raise_exc
        self.requests = []

    async def request(self, subject, payload, timeout=1.0):
        self.requests.append((subject, payload.decode()))
        if self.raise_exc is not None:
            raise self.raise_exc
        return FakeReply(self.reply)


def make_hedger(nc=None):
    return DeskHedger(nc or FakeNC())


def armed_hedger(nc=None, bid=598, ask=602):
    """Hedger with a valid two-sided AAH6 BBO cached."""
    h = make_hedger(nc)
    h.bbo["AAH6"] = {"bid": bid, "bid_vol": 20, "ask": ask, "ask_vol": 15}
    return h


def exec_msg(feed, incoming, resting, vol, price, aggressor, watched=None):
    """Build an E (execution) message as the exchange publishes it."""
    data = f"1000 E {incoming} {resting} {vol} {price} 0000000001 {aggressor}"
    subject = f"ex.md.{feed}.{watched or incoming.split(':')[0]}"
    return FakeMsg(data, subject)


# --------------------------------------------------------------------------- #
# Position tracking from executions
# --------------------------------------------------------------------------- #
class TestOnExecution:
    def test_incoming_aggressor_buy_adds_position(self):
        h = make_hedger()
        msg = exec_msg("AAH6", f"{QUOTER}:00000001", "MOVER001:00000001", 5, 600, "B")
        run(h.on_execution(msg, QUOTER))
        assert h.positions[QUOTER]["AAH6"] == 5

    def test_incoming_aggressor_sell_subtracts_position(self):
        h = make_hedger()
        msg = exec_msg("AAH6", f"{QUOTER}:00000001", "MOVER001:00000001", 5, 600, "S")
        run(h.on_execution(msg, QUOTER))
        assert h.positions[QUOTER]["AAH6"] == -5

    def test_resting_sold_when_aggressor_buys(self):
        h = make_hedger()
        msg = exec_msg("AAH6", "MOVER001:00000001", f"{QUOTER}:00000002", 5, 600, "B")
        run(h.on_execution(msg, QUOTER))
        assert h.positions[QUOTER]["AAH6"] == -5

    def test_resting_bought_when_aggressor_sells(self):
        h = make_hedger()
        msg = exec_msg("AAH6", "MOVER001:00000001", f"{QUOTER}:00000002", 5, 600, "S")
        run(h.on_execution(msg, QUOTER))
        assert h.positions[QUOTER]["AAH6"] == 5

    def test_foreign_execution_ignored(self):
        h = make_hedger()
        msg = exec_msg("AAH6", "MOVER001:00000001", f"{TAKER}:00000002", 5, 600, "B")
        run(h.on_execution(msg, QUOTER))
        assert h.desk_total() == 0

    def test_executions_accumulate(self):
        h = make_hedger()
        run(h.on_execution(
            exec_msg("AAH6", f"{TAKER}:00000001", "MOVER001:00000001", 5, 600, "B"), TAKER))
        run(h.on_execution(
            exec_msg("AAH6", f"{TAKER}:00000002", "MOVER001:00000001", 3, 601, "B"), TAKER))
        assert h.positions[TAKER]["AAH6"] == 8

    def test_feeds_tracked_separately(self):
        h = make_hedger()
        run(h.on_execution(
            exec_msg("AAH6", f"{TAKER}:00000001", "MOVER001:00000001", 5, 600, "B"), TAKER))
        run(h.on_execution(
            exec_msg("AAM6", f"{TAKER}:00000002", "MOVER001:00000002", 3, 615, "S"), TAKER))
        assert h.positions[TAKER]["AAH6"] == 5
        assert h.positions[TAKER]["AAM6"] == -3

    def test_short_payload_ignored(self):
        h = make_hedger()
        run(h.on_execution(FakeMsg("1000 E onlythree", "ex.md.AAH6.QUOTE001"), QUOTER))
        run(h.on_execution(FakeMsg("", "ex.md.AAH6.QUOTE001"), QUOTER))
        assert h.desk_total() == 0

    def test_non_execution_types_ignored(self):
        h = make_hedger()
        msg = FakeMsg("1000 T MOV001:00000001 QUOTE001:00000001 5 600 0000000001 B",
                      "ex.md.AAH6.QUOTE001")
        run(h.on_execution(msg, QUOTER))
        assert h.desk_total() == 0

    def test_unknown_feed_ignored(self):
        h = make_hedger()
        msg = exec_msg("ZZZ9", f"{QUOTER}:00000001", "MOVER001:00000001", 5, 600, "B")
        run(h.on_execution(msg, QUOTER))
        assert h.desk_total() == 0


# --------------------------------------------------------------------------- #
# BBO parsing
# --------------------------------------------------------------------------- #
class TestOnBbo:
    def test_bbo_parsed(self):
        h = make_hedger()
        run(h.on_bbo(FakeMsg("1000 AAH6 598 20 602 15", "ex.bbo.AAH6")))
        assert h.bbo["AAH6"] == {"bid": 598, "bid_vol": 20, "ask": 602, "ask_vol": 15}

    def test_bbo_empty_sides(self):
        h = make_hedger()
        run(h.on_bbo(FakeMsg("1000 AAH6 - 0 - 0", "ex.bbo.AAH6")))
        assert h.bbo["AAH6"]["bid"] is None
        assert h.bbo["AAH6"]["ask"] is None
        assert h.bbo["AAH6"]["bid_vol"] == 0
        assert h.bbo["AAH6"]["ask_vol"] == 0

    def test_bbo_short_payload_ignored(self):
        h = make_hedger()
        run(h.on_bbo(FakeMsg("1000 AAH6", "ex.bbo.AAH6")))
        assert h.bbo["AAH6"]["bid"] is None
        assert h.bbo["AAH6"]["ask"] is None

    def test_bbo_unknown_feed_ignored(self):
        h = make_hedger()
        run(h.on_bbo(FakeMsg("1000 ZZZ9 598 20 602 15", "ex.bbo.ZZZ9")))
        assert "ZZZ9" not in h.bbo


# --------------------------------------------------------------------------- #
# Position aggregation
# --------------------------------------------------------------------------- #
class TestAggregation:
    def test_desk_position_by_feed(self):
        h = make_hedger()
        h.positions[QUOTER]["AAH6"] = 7
        h.positions[TAKER]["AAH6"] = -3
        h.positions[HEDGER]["AAH6"] = 1
        assert h.desk_position_by_feed("AAH6") == 5

    def test_desk_total_sums_all_feeds_and_senders(self):
        h = make_hedger()
        h.positions[QUOTER]["AAH6"] = 10
        h.positions[TAKER]["AAM6"] = -4
        h.positions[HEDGER]["AAU6"] = 2
        assert h.desk_total() == 8

    def test_sender_total(self):
        h = make_hedger()
        h.positions[QUOTER]["AAH6"] = 3
        h.positions[QUOTER]["AAM6"] = -2
        h.positions[TAKER]["AAH6"] = 99
        assert h.sender_total(QUOTER) == 1


# --------------------------------------------------------------------------- #
# Order ids
# --------------------------------------------------------------------------- #
class TestNextOid:
    def test_oid_is_8_digit_chars_and_unique(self):
        h = make_hedger()
        seen = set()
        for _ in range(100):
            oid = h.next_oid()
            assert len(oid) == 8
            assert oid.isdigit()
            assert oid not in seen
            seen.add(oid)

    def test_oid_increments(self):
        h = make_hedger()
        a, b = h.next_oid(), h.next_oid()
        assert int(b) == int(a) + 1


# --------------------------------------------------------------------------- #
# Hedge decision (pure logic)
# --------------------------------------------------------------------------- #
class TestDecideHedge:
    def test_no_hedge_below_threshold(self):
        h = armed_hedger()
        h.positions[TAKER]["AAH6"] = hg.HEDGE_THRESHOLD - 1
        assert h.decide_hedge() is None
        h.positions[TAKER]["AAH6"] = -(hg.HEDGE_THRESHOLD - 1)
        assert h.decide_hedge() is None

    def test_no_hedge_at_zero(self):
        h = armed_hedger()
        assert h.decide_hedge() is None

    def test_long_desk_sells_clipped_qty(self):
        h = armed_hedger(bid=598, ask=602)
        h.positions[TAKER]["AAH6"] = 30
        assert h.decide_hedge() == ("AAH6", "S", 10, 595, 30)

    def test_short_desk_buys_clipped_qty(self):
        h = armed_hedger(bid=598, ask=602)
        h.positions[TAKER]["AAH6"] = -30
        assert h.decide_hedge() == ("AAH6", "B", 10, 605, -30)

    def test_exactly_at_threshold_triggers(self):
        h = armed_hedger(bid=598, ask=602)
        h.positions[TAKER]["AAH6"] = hg.HEDGE_THRESHOLD
        feed, side, qty, price, total = h.decide_hedge()
        assert (side, qty, price, total) == ("S", 10, 595, hg.HEDGE_THRESHOLD)

    def test_qty_respects_clip_setting(self, monkeypatch):
        monkeypatch.setattr(hg, "MAX_HEDGE_CLIP", 25)
        h = armed_hedger()
        h.positions[TAKER]["AAH6"] = 20
        assert h.decide_hedge()[2] == 15

    def test_no_hedge_without_bbo(self):
        h = make_hedger()
        h.positions[TAKER]["AAH6"] = 30
        assert h.decide_hedge() is None

    def test_no_hedge_with_one_sided_bbo(self):
        h = make_hedger()
        h.positions[TAKER]["AAH6"] = 30
        h.bbo["AAH6"] = {"bid": 598, "bid_vol": 20, "ask": None, "ask_vol": 0}
        assert h.decide_hedge() is None
        h.bbo["AAH6"] = {"bid": None, "bid_vol": 0, "ask": 602, "ask_vol": 15}
        assert h.decide_hedge() is None

    def test_position_across_senders_and_feeds_counts(self):
        h = armed_hedger()
        h.positions[QUOTER]["AAH6"] = 10
        h.positions[TAKER]["AAM6"] = 8
        feed, side, qty, price, total = h.decide_hedge()
        assert (side, total) == ("S", 18)
        assert qty == 10

    def test_offsetting_positions_net_out(self):
        h = armed_hedger()
        h.positions[QUOTER]["AAM6"] = 20
        h.positions[TAKER]["AAH6"] = -20
        assert h.decide_hedge() is None


# --------------------------------------------------------------------------- #
# Hedge loop (order emission, reply handling)
# --------------------------------------------------------------------------- #
class TestHedgeLoop:
    @staticmethod
    def drive(h, monkeypatch):
        monkeypatch.setattr(hg, "HEDGE_COOLDOWN_S", 0.005)

        async def _drive():
            task = asyncio.create_task(h.hedge_loop())
            await asyncio.sleep(0.05)
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

        asyncio.run(_drive())

    def test_sends_sell_order_when_long(self, monkeypatch):
        nc = FakeNC(reply=b"EXCHANGE Y 10")
        h = armed_hedger(nc)
        h.positions[TAKER]["AAH6"] = 30
        self.drive(h, monkeypatch)
        assert len(nc.requests) >= 1
        subject, payload = nc.requests[0]
        assert subject == f"ex.req.{HEDGER}"
        parts = payload.split()
        assert parts[0] == HEDGER
        assert parts[1] == "A"
        assert parts[2] == "AAH6"
        assert len(parts[3]) == 8 and parts[3].isdigit()
        assert parts[4] == "S"
        assert parts[5] == "10"
        assert parts[6] == "595"
        assert parts[7] == "F"

    def test_sends_buy_order_when_short(self, monkeypatch):
        nc = FakeNC(reply=b"EXCHANGE Y 10")
        h = armed_hedger(nc)
        h.positions[TAKER]["AAH6"] = -30
        self.drive(h, monkeypatch)
        assert len(nc.requests) >= 1
        parts = nc.requests[0][1].split()
        assert parts[4] == "B"
        assert parts[6] == "605"

    def test_no_orders_below_threshold(self, monkeypatch):
        nc = FakeNC()
        h = armed_hedger(nc)
        h.positions[TAKER]["AAH6"] = 5
        self.drive(h, monkeypatch)
        assert nc.requests == []

    def test_accept_reply_counts_as_hedge_fill(self, monkeypatch):
        nc = FakeNC(reply=b"EXCHANGE Y 10")
        h = armed_hedger(nc)
        h.positions[TAKER]["AAH6"] = 30
        self.drive(h, monkeypatch)
        assert h.hedge_fills >= 1

    def test_reject_reply_no_crash_no_fill(self, monkeypatch):
        nc = FakeNC(reply=b"EXCHANGE N 307 rate limit exceeded")
        h = armed_hedger(nc)
        h.positions[TAKER]["AAH6"] = 30
        self.drive(h, monkeypatch)
        assert len(nc.requests) >= 1
        assert h.hedge_fills == 0

    def test_request_timeout_no_crash(self, monkeypatch):
        nc = FakeNC(raise_exc=asyncio.TimeoutError())
        h = armed_hedger(nc)
        h.positions[TAKER]["AAH6"] = 30
        self.drive(h, monkeypatch)
        assert len(nc.requests) >= 1
        assert h.hedge_fills == 0
