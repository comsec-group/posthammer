class BankStats {
  readonly bankFunctions: number[];
  readonly rowMask: number;
  readonly columnMask: number; /* Derived, not parsed */

  constructor(rawFile: string) {
    const lines = rawFile.split("\n");
    const parse = (line: string): line =>
      line
        .split(",")
        .slice(1)
        .map((x) => parseInt(x, 16));

    this.bankFunctions = parse(lines[1]);
    this.rowMask = parse(lines[2])[0];
    this.columnMask = this.deriveColumnMask();
  }

  private deriveColumnMask() {
    for (const bf of this.bankFunctions) {
      if (!(bf & this.rowMask)) {
        let mostSignificant = bf.toString(2).split("").reverse().length - 1;
        return ((1 << mostSignificant) - 1) ^ ((1 << NUM_BUS_BITS) - 1);
      }
    }
  }

  /* Offset to bank */
  ptoba(p: number): number {
    assert(!(p & ~HUGE_PAGE_MASK));

    let bank = 0;

    for (let i = 0; i < this.bankFunctions.length; i++) {
      let bit: number = nzb(p & this.bankFunctions[i]) % 2;
      bank = bank ^ (bit << i);
    }

    return bank;
  }

  /* Page offset to row */
  ptoro(p: number): number {
    assert(!(p & ~HUGE_PAGE_MASK));
    return (p & this.rowMask) >> tzb(this.rowMask);
  }

  ptoco(p: number): number {
    return (p & this.columnMask) >> tzb(this.columnMask);
  }

  get rowCount(): number {
    return 1 << nzb(this.rowMask);
  }

  get columnCount(): number {
    return 1 << nzb(this.columnMask);
  }

  private deriveBank(es: EvictionSet, hugePageAtPages: number[]) {
    let banks = es.addrs
      .map((b) => this.ptoba(btop(b, hugePageAtPages).p))
      .filter((b, i, bs) => bs.indexOf(b) === i);
    assert(banks.length === 1);
    return banks[0];
  }

  private setBankKeepRow(p: number, bank: number) {
    let mask = this.bankFunctions.reduce((l, r) => l | r);

    /* Extract the bits that does not overlap with the row and columns masks */
    mask = (mask & this.rowMask) ^ mask;
    mask = (mask & this.columnMask) ^ mask;

    for (let i = 0; i < 1 << nzb(mask); i++) {
      let mod = i << tzb(mask);
      if (this.ptoba(p ^ mod) === bank) {
        assert(this.ptoro(p) === this.ptoro(p ^ mod));
        return p ^ mod;
      }
    }

    /* Cannot change bank! */
    assert(0);
  }

  private setColumnKeepBank(p: number, column: number): number {
    let ifSetMask = 0;
    let correctMask = 0;

    for (const bf of this.bankFunctions) {
      if (bf & this.columnMask) {
        ifSetMask = bf & this.columnMask;
        correctMask = ifSetMask ^ bf;
      }
    }

    let columnMask = column << tzb(this.columnMask);

    p ^= columnMask;

    if (columnMask & ifSetMask) p ^= correctMask;

    p = p & HUGE_PAGE_MASK;

    return p;
  }

  rowsDo(
    pair: EvictionSet,
    fnVictim: (b: number, column: number) => void | null,
    fnAggressor: (b: number, column: number) => void | null,
    hugePageAtPages: number[],
    li: LLCInfo,
  ) {
    assert(pair.partner);
    assert(pair.iterMask);
    assert(pair.iter.length === pair.partner.iter.length);
    assert(fnVictim);

    const bank = this.deriveBank(pair, hugePageAtPages);

    const setRow = (p, row) => {
      return (
        (p ^ ((row ^ this.ptoro(p)) << tzb(this.rowMask))) & HUGE_PAGE_MASK
      );
    };

    for (let i = 0; i < pair.iter.length; i++) {
      let miss: boolean = pair.iterMask & (1 << i % pair.laneSize);

      if (miss) {
        let x = btop(pair.iter[i], hugePageAtPages);
        let y = btop(pair.partner.iter[i], hugePageAtPages);

        assert(x.h === y.h);

        let rows = [x, y].map((z) => this.ptoro(z.p));

        assert(Math.abs(rows.reduce((l, r) => l - r)) === 2);

        let victimRows = rows.map((x) => [x + 1, x - 1]).flat();

        /*
         * The maximum row is decided by the overlap between the row and huge
         * page masks. For example, if we have 3 bits there, we can only
         * address rows 0-7: the others will be on different huge pages
         */
        victimRows = victimRows.filter(
          (x) => x >= 0 && x < 1 << nzb(this.rowMask & HUGE_PAGE_MASK),
        );
        victimRows = victimRows.filter((x, i, xs) => xs.indexOf(x) === i);

        for (const [i, vr] of victimRows.entries()) {
          let base = 0;

          base = setRow(base, vr);
          base = this.setBankKeepRow(base, bank);

          assert(this.ptoro(base) === vr);
          assert(this.ptoba(base) === bank);

          let xs: number[] = [];

          for (let c = 0; c < this.columnCount; c++) {
            /* We can use buf64 here, bus offset! */
            let p = this.setColumnKeepBank(base, c);
            let b = ptob(p, x.h, hugePageAtPages);
            let r = this.ptoro(btop(b, hugePageAtPages).p);

            assert(rows.includes(r + 1) || rows.includes(r - 1));
            assert(btop(b, hugePageAtPages).h === x.h);
            assert((b & 0x7) === 0);

            fnVictim(b, c);
            fnVictim(b + 4, c);

            if (fnAggressor) {
              let a = i === 0 ? x : y;
              let ba = ptob(this.setColumnKeepBank(a.p), a.h, hugePageAtPages);
              fnAggressor(ba, c);
              fnAggressor(ba + 4, c);
            }
          }
        }
      }
    }
  }

  mutateRowPreserveBank(p: number): number {
    let mask = 1 << (tzb(this.rowMask) + 1);

    for (const bf of this.bankFunctions) {
      mask = mask & bf ? mask | bf : mask;
    }

    return p ^ mask;
  }
}
