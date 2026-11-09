#!/usr/bin/env python3
"""
Market Tick Stream Analytics Worker (Python 3)
Consumes binary tick stream emitted by C generator and calculates VWAP & volatility metrics.
"""

import struct
import math
import json
import time

def main():
    print("\n=================================================================")
    print("     Market Tick Stream Analytics Engine (Python 3)             ")
    print("=================================================================\n")

    bin_path = "ticks.bin"
    with open(bin_path, "rb") as f:
        header = f.read(12)
        if len(header) < 12:
            raise ValueError("Corrupt tick stream header")
        
        magic, total_ticks, tick_size = struct.unpack("<III", header)
        if magic != 0x5449434B:
            raise ValueError(f"Invalid magic: {hex(magic)}")
        
        print(f"[Input] Processing {total_ticks} ticks (record size: {tick_size} bytes)...")

        # Format: uint64 timestamp_ns, 8s symbol, double price, uint32 volume, uint8 side
        fmt = "<Q8sdIB"
        expected_size = struct.calcsize(fmt)
        if expected_size != tick_size:
            raise ValueError(f"Struct size mismatch: expected {expected_size}, got {tick_size}")

        stats = {}
        t0 = time.perf_counter()

        # Stream parse
        raw_data = f.read()
        records_read = len(raw_data) // tick_size

        for i in range(records_read):
            offset = i * tick_size
            ts_ns, sym_bytes, price, vol, side = struct.unpack_from(fmt, raw_data, offset)
            sym = sym_bytes.decode("ascii").rstrip("\x00")

            if sym not in stats:
                stats[sym] = {
                    "count": 0,
                    "total_vol": 0,
                    "dollar_vol": 0.0,
                    "min_price": price,
                    "max_price": price,
                    "prices": [],
                    "buy_vol": 0,
                    "sell_vol": 0
                }
            
            s = stats[sym]
            s["count"] += 1
            s["total_vol"] += vol
            s["dollar_vol"] += price * vol
            if price < s["min_price"]: s["min_price"] = price
            if price > s["max_price"]: s["max_price"] = price
            if side == 1:
                s["buy_vol"] += vol
            else:
                s["sell_vol"] += vol
            
            # Subsample prices for standard deviation calculation
            if i % 10 == 0:
                s["prices"].append(price)

        elapsed = (time.perf_counter() - t0) * 1000.0

    print(f"[Processed] {records_read} ticks analyzed in {elapsed:.2f} ms")
    print(f"[Throughput] {(records_read / (elapsed / 1000.0)):.0f} ticks/second\n")
    print("┌──────────┬──────────┬──────────────┬────────────┬────────────┬─────────────┐")
    print("│ Symbol   │ Ticks    │ Volume       │ VWAP ($)   │ Min / Max  │ Buy/Sell    │")
    print("├──────────┼──────────┼──────────────┼────────────┼────────────┼─────────────┤")

    report_data = {
        "pipeline": "hybrid_c_python_ticks",
        "total_ticks": records_read,
        "processing_time_ms": round(elapsed, 2),
        "symbols": {}
    }

    for sym, s in sorted(stats.items()):
        vwap = s["dollar_vol"] / s["total_vol"] if s["total_vol"] > 0 else 0.0
        n_p = len(s["prices"])
        mean_p = sum(s["prices"]) / n_p if n_p > 0 else 0.0
        var_p = sum((p - mean_p) ** 2 for p in s["prices"]) / n_p if n_p > 0 else 0.0
        std_p = math.sqrt(var_p)
        buy_ratio = (s["buy_vol"] / s["total_vol"] * 100.0) if s["total_vol"] > 0 else 50.0

        print(f"│ {sym:<8} │ {s['count']:<8} │ {s['total_vol']:<12} │ ${vwap:<9.2f} │ ${s['min_price']:.1f}/${s['max_price']:.1f} │ {buy_ratio:>4.1f}% buy │")

        report_data["symbols"][sym] = {
            "tick_count": s["count"],
            "total_volume": s["total_vol"],
            "vwap": round(vwap, 3),
            "min_price": round(s["min_price"], 2),
            "max_price": round(s["max_price"], 2),
            "volatility_std": round(std_p, 3),
            "buy_volume_pct": round(buy_ratio, 2)
        }

    print("└──────────┴──────────┴──────────────┴────────────┴────────────┴─────────────┘\n")

    summary_file = "analytics_summary.json"
    with open(summary_file, "w") as jf:
        json.dump(report_data, jf, indent=2)

    print(f"[✓] Generated market analytics summary: '{summary_file}'")

if __name__ == "__main__":
    main()

