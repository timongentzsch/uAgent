// Keep lexical numbers and keys. Readable mode decodes string values for display;
// it is a text projection, never a serialization or a replacement for Source.
export function formatJSON(text: string, readable = false, nesting = 0) {
  JSON.parse(text);
  const tokens =
    text.match(/"(?:\\.|[^"\\])*"|[^\s{}\[\],:]+|[{}\[\],:]/g) || [];
  const parts: string[] = [];
  let depth = 0;
  const newline = () => parts.push("\n", "  ".repeat(depth));
  tokens.forEach((token, index) => {
    if (token === "{" || token === "[") {
      parts.push(token);
      depth++;
      if (tokens[index + 1] !== (token === "{" ? "}" : "]")) newline();
    } else if (token === "}" || token === "]") {
      depth--;
      if (tokens[index - 1] !== (token === "}" ? "{" : "[")) newline();
      parts.push(token);
    } else if (token === ",") {
      parts.push(token);
      newline();
    } else if (readable && token.startsWith('"') && tokens[index + 1] !== ":") {
      let value = JSON.parse(token) as string;
      if (nesting < 4 && /^[\s]*[\[{]/.test(value)) {
        try {
          value = formatJSON(value, true, nesting + 1);
        } catch {
          // Ordinary text that starts with a brace remains ordinary text.
        }
      }
      value = value.replace(
        /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/g,
        (character) => JSON.stringify(character).slice(1, -1),
      );
      parts.push(
        '"',
        value.replace(/\r\n|\r|\n/g, "\n" + "  ".repeat(depth)),
        '"',
      );
    } else parts.push(token === ":" ? ": " : token);
  });
  return parts.join("");
}
export function formatBody(text = "", readable = false) {
  try {
    return formatJSON(text, readable);
  } catch {
    return text;
  }
}
