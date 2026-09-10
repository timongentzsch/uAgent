// Copyright 2026 Timon Gentzsch

#include "include/ui/input_decoder.h"

#include <chrono>
#include <string>
#include <thread>

#include "tests/unit/test_support.h"

namespace uagent {

void TestTerminalInputDecoder() {
  TerminalInputDecoder decoder;
  decoder.Feed("\x1b[2");
  CHECK(!decoder.Next());
  constexpr char kPasted[] = "00~first\r\nsecond\rthird\0";
  decoder.Feed(std::string(kPasted, sizeof(kPasted) - 1));
  CHECK(!decoder.Next());
  decoder.Feed("\x1b[20");
  CHECK(!decoder.Next());
  decoder.Feed("1~");
  std::optional<TerminalInputToken> token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kPaste);
  CHECK(token && token->text == "first\nsecond\nthird");
  CHECK(token && !token->overflow);

  decoder.Feed("\x1b[");
  CHECK(!decoder.Next());
  decoder.Feed("O\x1b[I");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == "\x1b[O");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == "\x1b[I");

  decoder.Feed("\x1b");
  CHECK(!decoder.Next());
  CHECK(decoder.WakeDeadline().has_value());
  token = decoder.Next(true);
  CHECK(token && token->kind == TerminalInputTokenKind::kEscape);
  CHECK(!decoder.WakeDeadline().has_value());

  decoder.Feed("\x1bx");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == "\x1bx");

  decoder.Feed("\x1b");
  CHECK(!decoder.Next());
  std::this_thread::sleep_for(kInputEscapeDelay +
                              std::chrono::milliseconds(10));
  decoder.Feed("b");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kEscape);
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kText);
  CHECK(token && token->text == "b");

  decoder.Feed("\x1b\x1b");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kEscape);
  token = decoder.Next(true);
  CHECK(token && token->kind == TerminalInputTokenKind::kEscape);

  decoder.Feed("\x1b[");
  decoder.Feed(std::string(kInputSequenceBytes - 2, ';'));
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text.size() == kInputSequenceBytes);

  decoder.Feed("\x1b[200~");
  decoder.Feed(std::string(kInputPasteBytes + 1, 'x'));
  decoder.Feed("\x1b[201~");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kPaste);
  CHECK(token && token->overflow);
  CHECK(token && token->text.size() == kInputPasteBytes);

  // An oversized ordinary burst is discarded, then the next read is usable.
  TerminalInputDecoder overflow_decoder;
  overflow_decoder.Feed(std::string(kInputBufferBytes - 1, 'x'));
  overflow_decoder.Feed("xx");
  overflow_decoder.Feed("ignored");
  CHECK(overflow_decoder.HasReady());
  CHECK(!overflow_decoder.Next(true));
  CHECK(!overflow_decoder.HasReady());
  overflow_decoder.Feed("z");
  token = overflow_decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kText);
  CHECK(token && token->text == "z");

  // Terminal replies are not user input: the payload must not be typed into
  // the composer.
  decoder.Feed("\x1b]0;window title\x07");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == "\x1b]0;window title\x07");
  CHECK(!decoder.Next());

  // The other terminator: ST rather than BEL, here on a DCS payload.
  decoder.Feed("\x1bP1$r0m\x1b\\");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == "\x1bP1$r0m\x1b\\");
  CHECK(!decoder.Next());

  // Split across reads and across the Escape ambiguity window.
  decoder.Feed("\x1b]52;c;");
  CHECK(!decoder.Next());
  std::this_thread::sleep_for(kInputEscapeDelay +
                              std::chrono::milliseconds(10));
  decoder.Feed("YWJj\x07");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == "\x1b]52;c;YWJj\x07");
  CHECK(!decoder.Next());

  // Unterminated: bounded, dropped, and never emitted as text.
  decoder.Feed("\x1b]52;c;" + std::string(kInputStringSequenceBytes, 'A'));
  CHECK(!decoder.Next());
  decoder.Feed("z");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kText);
  CHECK(token && token->text == "z");
  CHECK(!decoder.Next());

  // X10 mouse: three raw bytes follow, one of which may be 0x1b.
  decoder.Feed("\x1b[M");
  CHECK(!decoder.Next());
  decoder.Feed("\x20\x1b\x21");
  token = decoder.Next();
  CHECK(token && token->kind == TerminalInputTokenKind::kSequence);
  CHECK(token && token->text == std::string("\x1b[M\x20\x1b\x21", 6));
  CHECK(!decoder.Next());

  CHECK(kInputHistoryEntries == 200);
  CHECK(kInputHistoryEntryBytes == size_t{16} * 1024);
  CHECK(kInputBufferBytes == size_t{64} * 1024);
  CHECK(ShouldRememberInput(" useful "));
  CHECK(!ShouldRememberInput(" /config user UAGENT_API_KEY=private-value"));
  CHECK(!ShouldRememberInput("/config\tuser UAGENT_API_KEY=private-value"));
  CHECK(!ShouldRememberInput(" \t\n"));
  CHECK(!ShouldRememberInput(std::string(kInputHistoryEntryBytes + 1, 'x')));
}

}  // namespace uagent
