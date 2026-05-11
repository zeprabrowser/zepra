// React Compatibility Test Suite
// Tests all patterns found in minified React UMD production bundles.
// All tests must print "PASS" — any other output indicates failure.

// === 1. for(x in obj) without var/let/const ===
var obj1 = {a: 1, b: 2, c: 3};
var keys1 = [];
var m;
for (m in obj1) {
    keys1.push(m);
}
console.log(keys1.length === 3 ? "PASS: for-in no var" : "FAIL: for-in no var");

// === 2. for(var x in obj) ===
var keys2 = [];
for (var k in obj1) {
    keys2.push(k);
}
console.log(keys2.length === 3 ? "PASS: for-in var" : "FAIL: for-in var");

// === 3. Labeled statement with for loop ===
var labelResult = 0;
outer: for (var i = 0; i < 5; i++) {
    for (var j = 0; j < 5; j++) {
        if (j === 2) break outer;
        labelResult++;
    }
}
console.log(labelResult === 2 ? "PASS: labeled break" : "FAIL: labeled break: " + labelResult);

// === 4. Labeled statement with single identifier (React minified: a:for(;;)) ===
var x4 = 0;
a: for (var i4 = 0; i4 < 3; i4++) {
    if (i4 === 1) break a;
    x4++;
}
console.log(x4 === 1 ? "PASS: single-char label" : "FAIL: single-char label: " + x4);

// === 5. void operator comparisons (React: void 0 !== x) ===
var undef5;
console.log(void 0 === undef5 ? "PASS: void 0 === undefined" : "FAIL: void 0 === undefined");
console.log(void 0 !== null ? "PASS: void 0 !== null" : "FAIL: void 0 !== null");

// === 6. typeof checks (React: "function"===typeof Symbol) ===
console.log("undefined" === typeof nonexistent6 ? "PASS: typeof undefined" : "FAIL: typeof undefined");
console.log("object" === typeof {} ? "PASS: typeof object" : "FAIL: typeof object");
console.log("function" === typeof console.log ? "PASS: typeof function" : "FAIL: typeof function");

// === 7. String coercion via concatenation (React: ""+b.key) ===
var val7 = 42;
console.log("" + val7 === "42" ? "PASS: string coercion" : "FAIL: string coercion");

// === 8. Comma operator in expressions (React: (a=1, b=2, c)) ===
var a8, b8;
var c8 = (a8 = 1, b8 = 2, a8 + b8);
console.log(c8 === 3 ? "PASS: comma operator" : "FAIL: comma operator: " + c8);

// === 9. Comma operator in for-loop update (React: for(i++,j++){}) ===
var i9 = 0, j9 = 10;
for (var x9 = 0; x9 < 3; x9++, j9--) {}
console.log(j9 === 7 ? "PASS: comma in for-update" : "FAIL: comma in for-update: " + j9);

// === 10. in operator outside for-loop ===
var obj10 = {foo: 1, bar: 2};
console.log("foo" in obj10 ? "PASS: in operator" : "FAIL: in operator");
console.log(!("baz" in obj10) ? "PASS: in operator neg" : "FAIL: in operator neg");

// === 11. IIFE pattern (React UMD wrapper) ===
var iife11 = (function(x) { return x * 2; })(21);
console.log(iife11 === 42 ? "PASS: IIFE" : "FAIL: IIFE: " + iife11);

// === 12. Ternary in complex expressions ===
var t12 = true ? "yes" : "no";
var f12 = false ? "yes" : "no";
console.log(t12 === "yes" && f12 === "no" ? "PASS: ternary" : "FAIL: ternary");

// === 13. Strict equality patterns (React: null===a) ===
console.log(null === null ? "PASS: null===null" : "FAIL: null===null");
console.log(!(null === undefined) ? "PASS: null!==undefined" : "FAIL: null!==undefined");

// === 14. Short-circuit logical (React: V&&a[V]) ===
var v14 = null;
var r14 = v14 && v14.prop;
console.log(r14 === null ? "PASS: short-circuit &&" : "FAIL: short-circuit &&");
var v14b = {prop: 42};
var r14b = v14b && v14b.prop;
console.log(r14b === 42 ? "PASS: short-circuit && truthy" : "FAIL: short-circuit && truthy");

// === 15. hasOwnProperty pattern (React: ba.hasOwnProperty.call(b, m)) ===
var obj15 = {x: 1};
console.log(obj15.hasOwnProperty("x") ? "PASS: hasOwnProperty" : "FAIL: hasOwnProperty");

// === 16. Multiple var declarations in for-init ===
for (var i16 = 0, len16 = 5; i16 < len16; i16++) {}
console.log(i16 === 5 && len16 === 5 ? "PASS: multi-var for" : "FAIL: multi-var for");

// === 17. Nested for-in with property check ===
var obj17 = {a: 1, b: 2};
var count17 = 0;
for (var p17 in obj17) {
    if (obj17.hasOwnProperty(p17)) count17++;
}
console.log(count17 === 2 ? "PASS: for-in hasOwnProp" : "FAIL: for-in hasOwnProp");

// === 18. Prototype chain in operator ===
function Base18() {}
Base18.prototype.inherited = true;
var inst18 = new Base18();
console.log("inherited" in inst18 ? "PASS: in proto chain" : "FAIL: in proto chain");

console.log("ALL TESTS COMPLETE");
