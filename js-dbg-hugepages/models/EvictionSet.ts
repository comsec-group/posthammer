class EvictionSet {
  addrs: number[];
  iter: number[];
  iter32: number[];
  iterMask: number;
  laneSize: number;

  readonly sizeWhenMinimal: number;

  private baseline: number;
  private partner: EvictionSet;

  constructor(sizeWhenMinimal: number) {
    this.addrs = [];
    this.iter = [];
    this.iter32 = [];
    this.iterMask = 0;
    this.laneSize = 0;

    this.sizeWhenMinimal = sizeWhenMinimal;

    this.baseline = 0;
    this.partner = null;
  }

  /* Simply loop over the set */
  private evict(): void {
    for (let i = 0; i < this.sizeWhenMinimal; i++) {
      dummy[0] = buf8[this.addrs[i]];
    }
  }

  install(dataPattern: number) {
    assert(this.iter32.length);

    for (let i = 0; i < this.iter32.length; i++) {
      assert(typeof buf32[this.iter32[i]] === "number");
      assert(
        !buf32[this.iter32[i]] ||
          buf32[this.iter32[i]] === dataPattern ||
          (buf32[this.iter32[i]] === dataPattern) ^ 0xffffffff,
      );
      buf32[this.iter32[i]] = this.iter32[(i + 1) % this.iter32.length];
    }
  }

  install2(dataPattern: number) {
    assert(this.iter32.length);
    assert(this.iter32.length % 2 === 0);

    for (let i = 0; i < this.iter32.length; i += 2) {
      assert(typeof buf32[this.iter32[i]] === "number");
      assert(
        !buf32[this.iter32[i]] ||
          buf32[this.iter32[i]] === dataPattern ||
          (buf32[this.iter32[i]] === dataPattern) ^ 0xffffffff,
      );
      buf32[this.iter32[i]] = this.iter32[(i + 2) % this.iter32.length];
      buf32[this.iter32[i + 1]] = this.iter32[(i + 3) % this.iter32.length];
    }
  }

  uninstall() {
    assert(this.iter32.length);

    for (let i = 0; i < this.iter32.length; i++) {
      assert(typeof buf32[this.iter32[i]] === "number");
      /* Why did this fail once?! Unlucky overlapping double pattern maybe */
      assert(buf32[this.iter32[i]]);
      buf32[this.iter32[i]] = 0;
    }
  }

  get sets(): EvictionSet[] {
    assert(this.partner);
    return [this, this.partner];
  }

  get start(): number {
    assert(this.iter32.length);
    return this.iter32[0];
  }

  pair(set: EvictionSet) {
    if (this.partner === null) {
      this.partner = set;
      set.pair(this);
    } else {
      assert(this.partner == set);
    }
  }

  printIterable(hugePageAtPages: number[], li: LLCInfo, bs: BankStats) {
    assert(this.partner);
    assert(this.iter.length % this.laneSize === 0);

    const lanes: number = this.iter.length / this.laneSize;

    let xs: number[] = this.iter;
    let ys: number[] = this.partner.iter;

    const shifted = (zs) => {
      return zs.map((x) => x >> 6);
    };

    const xss = shifted(xs);
    const yss = shifted(ys);

    print(
      "ln|gli|li|m|         b| h |bk|row|set| ix|---|m|         b| h |bk|row|set| ix|",
    );

    for (let l = 0; l < lanes; l++) {
      for (let i = 0; i < this.laneSize; i++) {
        if (l && !i) {
          print("");
        }

        let k = l * this.laneSize + i;
        let x = btop(xs[k], hugePageAtPages);
        let y = btop(ys[k], hugePageAtPages);
        let miss = this.iterMask & (1 << i) ? "*" : " ";

        print(
          __pfst(l, 0, 2),
          __pfst(k, 0, 3),
          __pfst(i, 0, 2),
          miss,
          __pfst(xs[k], 0, 10),
          __pfst(x.h, 0, 3),
          __pfst(bs.ptoba(x.p), 0, 2),
          __pfst(bs.ptoro(x.p), 0, 3),
          __pfst(li.ptose(x.p), 0, 3),
          __pfst(xss.indexOf(xss[k]), 0, 3),
          "   ",
          miss,
          __pfst(ys[k], 0, 10),
          __pfst(y.h, 0, 3),
          __pfst(bs.ptoba(y.p), 0, 2),
          __pfst(bs.ptoro(y.p), 0, 3),
          __pfst(li.ptose(y.p), 0, 3),
          __pfst(yss.indexOf(yss[k]), 0, 3),
        );
      }
    }
  }

  /*
   * nonUniformity: 0, 1, 2, 3
   */
  newIterable(mask: number, laneSize: number, nonUniformity: number) {
    const intSizeInBytes: number = 4;

    /* A, B, C or just A, B */
    const uniqueLanes = nonUniformity ? MAX_UNIQUE_LANES : MAX_UNIQUE_LANES - 1;

    assert(nonUniformity >= 0 && nonUniformity <= 3);
    assert(uniqueLanes * laneSize <= this.addrs.length);

    /* In total, e.g. A, B, A, B, A, B, A, C */
    let lanes: number = 2 + 2 * nonUniformity;
    let size: number = laneSize * lanes;

    this.iter = Array(size).fill(0);
    this.iterMask = mask;
    this.laneSize = laneSize;

    for (let l = 0; l < lanes; l++) {
      for (let j = 0; j < laneSize; j++) {
        let i = l * laneSize + j;

        if (nonUniformity && l == lanes - 1) {
          this.iter[i] = this.addrs[j + 2 * laneSize]; /* C */
        } else if (l % 2 == 1) {
          this.iter[i] = this.addrs[j + laneSize]; /* B */
        } else {
          this.iter[i] = this.addrs[j]; /* A */
        }

        /* Hit in a non-first lane: always A! */
        if (l && !(mask & (1 << j))) {
          this.iter[i] = this.addrs[j];
        }

        this.iter[i] += l * intSizeInBytes;
      }
    }

    this.iter32 = this.iter.map((x) => {
      assert((x & 0x3) === 0);
      assert(!(~buf32Bounds & (x >> 2)));
      return x >> 2;
    });
  }

  public evicts() {
    const reasonableUpperLimit: number = 1000;
    const reps: number = 8;

    for (const [i, base] of [0, this.sizeWhenMinimal].entries()) {
      let times: number[] = Array(reps).fill(0);
      let m: number = reasonableUpperLimit;

      if (!i && this.baseline) continue;

      /* We need to get this code optimized first */
      while (m >= reasonableUpperLimit) {
        for (let r = 0; r < reps; r++) {
          var before: number = performance.now();

          for (let a = 0; a < 2 * 10 ** 4; a++) {
            this.evict();

            /*
             * These four accesses should be slow (cache misses) if the
             * eviction worked, or fast if we have baseline === true
             */
            dummy[0] ^= buf8[this.addrs[base]];
            dummy[0] ^= buf8[this.addrs[base + 1]];
            dummy[0] ^= buf8[this.addrs[base + 2]];
            dummy[0] ^= buf8[this.addrs[base + 3]];
          }

          var after: number = performance.now();
          times[r] = after - before;
        }

        m = median(times);
      }

      if (!i) {
        this.baseline = m;
      } else {
        const factor = 3;
        return { median: m, evicts: m >= factor * this.baseline };
      }
    }

    /* Unreachable */
    assert(0);
  }
}
