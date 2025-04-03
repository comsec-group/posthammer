/**
 * __pfst: print fast. Easy way to get the rounding and digits right
 */
const __pfst = (x: number, digits: number, pad: number) =>
  x.toFixed(digits).toString().padStart(pad, " ");

/**
 * Merges two arrays by interleaving them.
 *
 * @param {Array} arr1 - The first array.
 * @param {Array} arr2 - The second array.
 * @returns {Array} The merged array.
 */
function interleave(arr1: any[], arr2: any[]): any[] {
  assert(arr1.length === arr2.length, "Arrays must have the same length.");
  let result = [];
  for (let i = 0; i < arr1.length; i++) {
    result.push(arr1[i]);
    result.push(arr2[i]);
  }
  return result;
}

/**
 * Merges two arrays by interleaving them.
 *
 * @param {string} str1 - The first string.
 * @param {string} str2 - The second string.
 * @returns {string} The merged string.
 */
function interleaveStrings(str1: string, str2: string): string {
  if (typeof str1 !== "string" || typeof str2 !== "string") {
    throw new Error("Arguments must be strings.");
  }

  let result = [];
  for (let i = 0; i < str1.length; i++) {
    result.push(str1[i]);
    result.push(str2[i]);
  }
  return result.join("");
}

/* Nonzero bits */
function nzb(x: number): number {
  return x
    .toString(2)
    .split("")
    .filter((x) => x === "1").length;
}

/**
 * Counts the number of trailing zeros in the binary representation of a number
 *
 * @param {number} num - The input number
 * @returns {number} - The number of trailing zeros
 */
function tzb(num: number): number {
  if (num === 0) {
    return Number.MAX_SAFE_INTEGER.toString(2).length;
  }

  let count = 0;

  while ((num & 1) === 0) {
    num >>= 1;
    count++;
  }

  return count;
}

/**
 * Returns the binary representation of a given number.
 *
 * @param{number} dec - The number to be converted to binary.
 * @returns {string} The binary string.
 */
function dec2bin(dec: number): string {
  return (dec >>> 0).toString(2);
}

/**
 * Calculates the median value of an array of numbers.
 *
 * @param {Array<number>} x - The array of numbers.
 * @returns {number} The median value.
 */
function median(x: Array<number>): number {
  x.sort();
  let y =
    x[Math.floor((x.length + 1) / 2) - 1] +
    x[Math.ceil((x.length + 1) / 2) - 1];
  return y / 2;
}

/**
 * Shuffles the elements in an array in place.
 *
 * @param {Array} array - The array to be shuffled.
 */
function shuffle(array: Array<any>): void {
  let currentIndex = array.length;
  // While there remain elements to shuffle...
  while (currentIndex != 0) {
    // Pick a remaining element...
    let randomIndex = Math.floor(rand() * currentIndex);
    currentIndex--;
    // And swap it with the current element.
    [array[currentIndex], array[randomIndex]] = [
      array[randomIndex],
      array[currentIndex],
    ];
  }
}

/* / 1000 because the arguments are obtained through performance.now() */
function timeInSeconds(after: number, before: number): number {
  return (after - before) / 1000;
}

/**
 * Reports the time elapsed since the last call to this function.
 *
 * @param {string} str - The description of the time measurement.
 */
function reportTime(str: string) {
  let timeNow = performance.now();
  let lap = timeInSeconds(timeNow, timeZero);
  let total = timeInSeconds(timeNow, firstTimeZero);

  if (VERBOSE) {
    console.log(
      "[Time sl/st/ml/mt] " + str,
      __pfst(lap, 2, 2),
      __pfst(total, 2, 2),
      __pfst(lap / 60, 2, 2),
      __pfst(total / 60, 2, 2),
    );
  } else if (str == "Bit flip") {
    console.log("(Bit flip)", total, "s");
  } else {
    console.log("    Done.", total.toFixed(1), "s");
  }

  timeZero = timeNow;
}

function assert(condition: any, message: string = "") {
  if (!condition) {
    throw new Error("Assertion failed:", message).stack;
  }
}

/*
 * Copied from
 * https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/random
 */
function getRandomInt(min: number, max: number): number {
  const minCeiled: number = Math.ceil(min);
  const maxFloored: number = Math.floor(max);
  return Math.floor(Math.random() * (maxFloored - minCeiled) + minCeiled);
}

function printAddresses(
  addresses: number[],
  header: string,
  condition: boolean,
  hugePageAtPages: number[],
  bs: bankStats,
  li: LLCInfo,
) {
  if (!condition) {
    return;
  }

  console.log(header);
  console.log("Address   ,    Slc, Set,  Bk,  Row");
  for (let i = 0; i < addresses.length; i++) {
    let p = btop(addresses[i], hugePageAtPages).p;
    let outstr: string = "";
    outstr +=
      `0x${addresses[i].toString(16).padStart(8, "0")}, ` +
      `${li.ptosl(p).toString(10).padStart(6, " ")}, ` +
      `${li.ptose(p).toString(10).padStart(3, " ")}, ` +
      `${bs ? bs.ptoba(p).toString(10).padStart(3, " ") : ""}, ` +
      `${bs ? bs.ptoro(p).toString(10).padStart(4, " ") : ""}`;
    console.log(outstr);
  }
}
