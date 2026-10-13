// The operations of Verilog one beside another, each to an output: a
// circuit for checking that examples/logic/words.py, which writes a
// circuit out word by word, gives every one of them the meaning the
// gates have (check/words.py). Divisors are kept from being 0 and the
// bits that are picked out inside their word: Verilog leaves open what
// comes of the others, and the gates and the words need not agree.
// (The multiplications are small: turning a big one into gates takes
// Yosys's abc minutes.)
module ops(
    input clk, input [31:0] a, input [31:0] b, input [7:0] c, input [6:0] s, input [2:0] k,
    output [31:0] sum, output [31:0] diff, output [19:0] prod, output [23:0] sprod,
    output [15:0] quo, output [15:0] rem, output [11:0] squo, output [11:0] srem,
    output [31:0] left, output [31:0] right, output [31:0] sright, output [47:0] sleft,
    output [39:0] far, output [15:0] sfar, output [3:0] part, output [7:0] spart,
    output [11:0] compares, output [7:0] reduces, output [31:0] bitwise,
    output [31:0] chosen, output [15:0] mixed, output [40:0] grown, output [8:0] negated,
    output [31:0] count, output [31:0] held, output [15:0] kept, output [31:0] word,
    output [12:0] stage);
  wire signed [31:0] sa = a;
  wire signed [31:0] sb = b;
  wire signed [7:0] sc = c;
  assign sum = a + b + c;
  assign diff = a - {b[15:0], c, c};
  assign prod = a[9:0] * b[7:0];
  assign sprod = $signed(a[9:0]) * $signed(b[7:0]);
  assign quo = a[15:0] / (b[15:0] | 16'd1);
  assign rem = a[15:0] % (b[9:0] | 10'd1);
  assign squo = $signed(a[11:0]) / $signed(b[7:0] | 8'd1);
  assign srem = $signed(a[11:0]) % $signed(b[7:0] | 8'd1);
  assign left = a << s[4:0];
  assign right = a >> s;
  assign sright = sa >>> s;
  assign sleft = sc <<< s[5:0];
  assign far = {c, a} >> b[7:0];
  assign sfar = sc >>> k;
  assign part = a[s[3:0] +: 4];
  assign spart = b[{1'b1, c[3:0]} -: 8];
  assign compares = {a < b, a <= b, a > b, a >= b, sa < sb, sa <= sb, sa > sb, sa >= sb,
                     a == b, a != b, sc < sa, c[3:0] == a[3:0]};
  assign reduces = {&a[7:0], |a, ^a, ~^b, !c, a && b, c[0] || k[1], &k};
  assign bitwise = (a & b) | (~a & {4{c}}) ^ (a ~^ {b[0], b[31:1]});
  reg [31:0] pick;
  always @* begin
    case (k)
      3'd0: pick = a;
      3'd1: pick = b;
      3'd2: pick = {24'b0, c};
      3'd3: pick = a ^ b;
      3'd5: pick = {a[15:0], b[31:16]};
      default: pick = 32'hdeadbeef;
    endcase
  end
  assign chosen = s[0] ? pick : ~pick;
  assign mixed = {c[3:0], a[31:28], 1'b1, b[2:0], k, 1'b0};
  assign grown = {sc, 1'b0} + sb;
  assign negated = -sc;

  // Flip-flops: one that counts, one that is set back, one that takes
  // only when it is told, one that starts with a value.
  reg [31:0] counter = 0;
  reg [31:0] total = 0;
  reg [15:0] saved = 16'h1234;
  reg [12:0] shift = 13'h1001;
  always @(posedge clk) begin
    counter <= counter + 1;
    if (k == 3'd7) total <= 0;
    else if (s[1]) total <= total + a;
    if (c[7]) saved <= saved ^ b[15:0];
    shift <= {shift[11:0], shift[12] ^ shift[3] ^ a[0]};
  end
  assign count = counter;
  assign held = total;
  assign kept = saved;
  assign stage = shift;

  // A memory: written a byte at a time, read where an input says.
  reg [31:0] memory [0:15];
  integer i;
  initial for (i = 0; i < 16; i = i + 1) memory[i] = i * 32'h01010101;
  always @(posedge clk) begin
    if (c[0]) memory[b[3:0]][7:0] <= a[7:0];
    if (c[1]) memory[b[3:0]][31:8] <= a[31:8];
  end
  assign word = memory[a[3:0]] + memory[s[3:0]];
endmodule
