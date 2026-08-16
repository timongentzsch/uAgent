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

  CHECK(kInputHistoryEntries == 200);
  CHECK(kInputHistoryEntryBytes == 16 * 1024);
  CHECK(kInputBufferBytes == 64 * 1024);
  CHECK(ShouldRememberInput(" useful "));
  CHECK(!ShouldRememberInput(" \t\n"));
  CHECK(!ShouldRememberInput(std::string(kInputHistoryEntryBytes + 1, 'x')));
}

}  // namespace uagent
