/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          markdown.cpp
        -\-    _|__
         |\___/  . \        Created on 26 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace qrprotec::markdown {

namespace {

bool is_space(char character) {
  return character == ' ' || character == '\t';
}

bool is_word(char character) {
  // octets UTF-8 non ASCII comptes comme lettres (é, à...)
  return std::isalnum(static_cast< unsigned char >(character)) || static_cast< unsigned char >(character) >= 0x80;
}

std::string trim(const std::string &text) {
  std::size_t begin = 0;
  std::size_t end   = text.size();
  while (begin < end && is_space(text[begin]))
    ++begin;
  while (end > begin && is_space(text[end - 1]))
    --end;
  return text.substr(begin, end - begin);
}

bool starts_with(const std::string &text, const std::string &prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

// Longueur de la suite de backticks commencant en `pos`
std::size_t backtick_run(const std::string &text, std::size_t pos) {
  std::size_t end = pos;
  while (end < text.size() && text[end] == '`')
    ++end;
  return end - pos;
}

// Fin (position du premier backtick fermant) d'un code `...` ouvert en `pos` par `run` backticks, npos sinon
std::size_t code_close(const std::string &text, std::size_t pos, std::size_t run) {
  std::size_t search = pos + run;
  while (search < text.size()) {
    const std::size_t found = text.find('`', search);
    if (found == std::string::npos)
      return std::string::npos;
    const std::size_t length = backtick_run(text, found);
    if (length == run)
      return found;
    search = found + length;
  }
  return std::string::npos;
}

// Position du delimiteur fermant `delim` (*, **, _, __) apres `from`, en sautant le code et les echappements
std::size_t find_closing(const std::string &text, std::size_t from, const std::string &delim) {
  for (std::size_t index = from; index < text.size();) {
    const char character = text[index];
    if (character == '\\') {
      index += 2;
      continue;
    }
    if (character == '`') {
      const std::size_t run   = backtick_run(text, index);
      const std::size_t close = code_close(text, index, run);
      index                   = close == std::string::npos ? index + run : close + run;
      continue;
    }
    if (text.compare(index, delim.size(), delim) == 0) {
      const bool single    = delim.size() == 1;
      const bool doubled   = index + 1 < text.size() && text[index + 1] == delim[0];
      const bool after_txt = index > from && !is_space(text[index - 1]);
      if (single && doubled) {
        // ** dans un *italique* : saute la paire
        index += 2;
        continue;
      }
      const bool word_after = index + delim.size() < text.size() && is_word(text[index + delim.size()]);
      if (after_txt && !(delim[0] == '_' && word_after))
        return index;
      index += delim.size();
      continue;
    }
    ++index;
  }
  return std::string::npos;
}

// Fin du libelle d'un lien [ ... ] ouvert en `pos` (position du ]), npos sinon
std::size_t bracket_close(const std::string &text, std::size_t pos) {
  int depth = 0;
  for (std::size_t index = pos; index < text.size(); ++index) {
    const char character = text[index];
    if (character == '\\') {
      ++index;
    } else if (character == '`') {
      const std::size_t run   = backtick_run(text, index);
      const std::size_t close = code_close(text, index, run);
      if (close != std::string::npos)
        index = close + run - 1;
    } else if (character == '[') {
      ++depth;
    } else if (character == ']' && --depth == 0) {
      return index;
    }
  }
  return std::string::npos;
}

std::string decode_entities(const std::string &text) {
  static const std::pair< const char *, const char * > kEntities[] = {
    { "&lt;", "<" }, { "&gt;", ">" }, { "&amp;", "&" }, { "&nbsp;", "\xC2\xA0" }, { "&quot;", "\"" },
  };
  std::string output;
  for (std::size_t index = 0; index < text.size(); ++index) {
    bool replaced = false;
    if (text[index] == '&')
      for (const auto &[entity, value] : kEntities)
        if (text.compare(index, std::char_traits< char >::length(entity), entity) == 0) {
          output += value;
          index += std::char_traits< char >::length(entity) - 1;
          replaced = true;
          break;
        }
    if (!replaced)
      output += text[index];
  }
  return output;
}

void parse_into(const std::string &text, const Span &style, std::vector< Span > &out) {
  std::string buffer;
  const auto  flush = [&]() {
    if (buffer.empty())
      return;
    Span span = style;
    span.text = decode_entities(buffer);
    out.push_back(span);
    buffer.clear();
  };
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character == '\\' && index + 1 < text.size() && std::ispunct(static_cast< unsigned char >(text[index + 1]))) {
      buffer += text[++index];
      continue;
    }
    if (character == '\n') {
      flush();
      Span line_break = style;
      line_break.text = "\n";
      out.push_back(line_break);
      continue;
    }
    if (character == '`') {
      const std::size_t run   = backtick_run(text, index);
      const std::size_t close = code_close(text, index, run);
      if (close == std::string::npos) {
        buffer += text.substr(index, run);
        index += run - 1;
        continue;
      }
      flush();
      std::string code = text.substr(index + run, close - index - run);
      if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
        code = code.substr(1, code.size() - 2);
      Span span = style;
      span.code = true;
      span.text = code;
      out.push_back(span);
      index = close + run - 1;
      continue;
    }
    if (character == '[') {
      const std::size_t close = bracket_close(text, index);
      if (close != std::string::npos && close + 1 < text.size() && text[close + 1] == '(') {
        int         depth = 0;
        std::size_t end   = std::string::npos;
        for (std::size_t search = close + 1; search < text.size(); ++search) {
          if (text[search] == '(')
            ++depth;
          else if (text[search] == ')' && --depth == 0) {
            end = search;
            break;
          }
        }
        if (end != std::string::npos) {
          flush();
          std::string target = trim(text.substr(close + 2, end - close - 2));
          // [texte](cible "titre") : titre ignore
          if (const std::size_t space = target.find(' '); space != std::string::npos)
            target = target.substr(0, space);
          if (target.size() >= 2 && target.front() == '<' && target.back() == '>')
            target = target.substr(1, target.size() - 2);
          Span link = style;
          link.link = target;
          parse_into(text.substr(index + 1, close - index - 1), link, out);
          index = end;
          continue;
        }
      }
    }
    if (character == '<') {
      const std::size_t close = text.find('>', index);
      if (close != std::string::npos) {
        const std::string inside = text.substr(index + 1, close - index - 1);
        if ((starts_with(inside, "http://") || starts_with(inside, "https://")) && inside.find(' ') == std::string::npos) {
          flush();
          Span link = style;
          link.link = inside;
          link.text = inside;
          out.push_back(link);
          index = close;
          continue;
        }
      }
    }
    if (character == '*' || character == '_') {
      const bool        doubled = index + 1 < text.size() && text[index + 1] == character;
      const std::string delim(doubled ? 2 : 1, character);
      const std::size_t open_end = index + delim.size();
      const bool        before_ok = character == '*' || index == 0 || !is_word(text[index - 1]);
      if (before_ok && open_end < text.size() && !is_space(text[open_end])) {
        const std::size_t close = find_closing(text, open_end, delim);
        if (close != std::string::npos) {
          flush();
          Span inner = style;
          (doubled ? inner.bold : inner.italic) = true;
          parse_into(text.substr(open_end, close - open_end), inner, out);
          index = close + delim.size() - 1;
          continue;
        }
      }
    }
    buffer += character;
  }
  flush();
}

std::size_t indent_of(const std::string &line) {
  std::size_t width = 0;
  for (const char character : line) {
    if (character == ' ')
      ++width;
    else if (character == '\t')
      width += 4 - width % 4;
    else
      break;
  }
  return width;
}

// Retire `count` colonnes d'indentation (espaces ou tabulations)
std::string dedent(const std::string &line, std::size_t count) {
  std::size_t index = 0;
  std::size_t width = 0;
  while (index < line.size() && width < count && is_space(line[index])) {
    width += line[index] == '\t' ? 4 - width % 4 : 1;
    ++index;
  }
  return line.substr(index);
}

bool is_rule(const std::string &trimmed) {
  if (trimmed.size() < 3)
    return false;
  const char  mark  = trimmed[0];
  std::size_t count = 0;
  if (mark != '-' && mark != '*' && mark != '_')
    return false;
  for (const char character : trimmed) {
    if (character == mark)
      ++count;
    else if (!is_space(character))
      return false;
  }
  return count >= 3;
}

// Element de liste : longueur du marqueur et de l'espace qui suit (0 si la ligne n'en est pas un)
std::size_t list_marker(const std::string &trimmed, std::string &marker) {
  std::size_t index = 0;
  if (!trimmed.empty() && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+')) {
    index  = 1;
    marker = "-";
  } else {
    while (index < trimmed.size() && index < 9 && std::isdigit(static_cast< unsigned char >(trimmed[index])))
      ++index;
    if (index == 0 || index >= trimmed.size() || (trimmed[index] != '.' && trimmed[index] != ')'))
      return 0;
    marker = trimmed.substr(0, index) + ".";
    ++index;
  }
  if (index == trimmed.size())
    return index;
  if (!is_space(trimmed[index]))
    return 0;
  std::size_t spaces = 0;
  while (index + spaces < trimmed.size() && is_space(trimmed[index + spaces]))
    ++spaces;
  if (index + spaces == trimmed.size())
    return index + spaces;
  return index + (spaces > 4 ? 1 : spaces);
}

bool is_table_separator(const std::string &trimmed) {
  if (trimmed.find('-') == std::string::npos)
    return false;
  for (const char character : trimmed)
    if (character != '|' && character != '-' && character != ':' && !is_space(character))
      return false;
  return true;
}

std::vector< std::vector< Span > > table_row(const std::string &line) {
  std::string row = trim(line);
  if (!row.empty() && row.front() == '|')
    row.erase(0, 1);
  if (!row.empty() && row.back() == '|' && (row.size() < 2 || row[row.size() - 2] != '\\'))
    row.pop_back();
  std::vector< std::vector< Span > > cells;
  std::string                        cell;
  for (std::size_t index = 0; index < row.size(); ++index) {
    const char character = row[index];
    if (character == '\\' && index + 1 < row.size() && row[index + 1] == '|') {
      cell += '|';
      ++index;
    } else if (character == '`') {
      const std::size_t run   = backtick_run(row, index);
      const std::size_t close = code_close(row, index, run);
      const std::size_t end   = close == std::string::npos ? index + run : close + run;
      // \| dans un code de tableau : | (comme GitHub)
      std::string code = row.substr(index, end - index);
      for (std::size_t bar = code.find("\\|"); bar != std::string::npos; bar = code.find("\\|", bar + 1))
        code.erase(bar, 1);
      cell += code;
      index = end - 1;
    } else if (character == '|') {
      cells.push_back(parse_inline(trim(cell)));
      cell.clear();
    } else {
      cell += character;
    }
  }
  cells.push_back(parse_inline(trim(cell)));
  return cells;
}

struct ListLevel {
  std::size_t indent;  // colonne du marqueur
  std::size_t content; // colonne du texte
};

std::vector< Block > parse_lines(const std::vector< std::string > &lines) {
  std::vector< Block >      blocks;
  std::vector< ListLevel >  stack;
  std::string               raw;          // texte du paragraphe ou de l'element de liste ouvert
  bool                      open  = false; // dernier bloc : paragraphe ou element de liste qui prend la suite
  bool                      blank = false; // ligne vide juste avant
  const auto                close = [&]() {
    if (open)
      blocks.back().spans = parse_inline(raw);
    open = false;
    raw.clear();
  };
  const auto append = [&](const std::string &text) {
    // deux espaces ou \ en fin de ligne : retour a la ligne force
    if (raw.size() >= 2 && raw.compare(raw.size() - 2, 2, "  ") == 0) {
      raw = trim(raw) + "\n";
    } else if (!raw.empty() && raw.back() == '\\') {
      raw.back() = '\n';
    } else if (!raw.empty()) {
      raw += ' ';
    }
    raw += text;
  };
  // ligne dans un element de liste (indentee sous son texte) : garde la liste
  const auto in_item = [&](std::size_t indent) { return !stack.empty() && indent >= stack.back().content; };

  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const std::string &line    = lines[line_index];
    const std::string  trimmed = trim(line);
    if (trimmed.empty()) {
      close();
      blank = true;
      continue;
    }
    const std::size_t indent      = indent_of(line);
    const bool        was_blank   = blank;
    blank                         = false;
    const bool        inside_item = in_item(indent);
    // bloc de code delimite
    if (starts_with(trimmed, "```") || starts_with(trimmed, "~~~")) {
      close();
      const std::string fence = trimmed.substr(0, 3);
      Block             block;
      block.kind  = BlockKind::Code;
      block.level = inside_item ? static_cast< int >(stack.size()) : 0;
      if (!inside_item)
        stack.clear();
      std::size_t end = line_index + 1;
      for (; end < lines.size(); ++end) {
        if (starts_with(trim(lines[end]), fence))
          break;
        block.code += (block.code.empty() && end == line_index + 1 ? "" : "\n") + dedent(lines[end], indent);
      }
      blocks.push_back(block);
      line_index = end;
      continue;
    }
    if (indent < 4 && trimmed[0] == '#') {
      std::size_t level = 0;
      while (level < trimmed.size() && trimmed[level] == '#')
        ++level;
      if (level <= 6 && (level == trimmed.size() || is_space(trimmed[level]))) {
        close();
        stack.clear();
        std::string text = trim(trimmed.substr(level));
        // # fermants facultatifs
        std::size_t end = text.size();
        while (end > 0 && text[end - 1] == '#')
          --end;
        if (end < text.size() && (end == 0 || is_space(text[end - 1])))
          text = trim(text.substr(0, end));
        Block block;
        block.kind  = BlockKind::Heading;
        block.level = static_cast< int >(level);
        block.spans = parse_inline(text);
        blocks.push_back(block);
        continue;
      }
    }
    if (indent < 4 && is_rule(trimmed)) {
      close();
      stack.clear();
      Block block;
      block.kind = BlockKind::Rule;
      blocks.push_back(block);
      continue;
    }
    if (trimmed[0] == '>') {
      close();
      std::vector< std::string > quoted;
      std::size_t                end = line_index;
      for (; end < lines.size(); ++end) {
        const std::string current = trim(lines[end]);
        if (current.empty() || current[0] != '>')
          break;
        std::string content = current.substr(1);
        if (!content.empty() && content[0] == ' ')
          content.erase(0, 1);
        quoted.push_back(content);
      }
      Block block;
      block.kind     = BlockKind::Quote;
      block.level    = inside_item ? static_cast< int >(stack.size()) : 0;
      block.children = parse_lines(quoted);
      if (!inside_item)
        stack.clear();
      blocks.push_back(block);
      line_index = end - 1;
      continue;
    }
    if (trimmed[0] == '|' && line_index + 1 < lines.size() && is_table_separator(trim(lines[line_index + 1]))) {
      close();
      stack.clear();
      Block block;
      block.kind   = BlockKind::Table;
      block.header = table_row(trimmed);
      std::size_t end = line_index + 2;
      for (; end < lines.size(); ++end) {
        const std::string current = trim(lines[end]);
        if (current.empty() || current[0] != '|')
          break;
        std::vector< std::vector< Span > > row = table_row(current);
        row.resize(block.header.size());
        block.rows.push_back(row);
      }
      blocks.push_back(block);
      line_index = end - 1;
      continue;
    }
    std::string       marker;
    const std::size_t marker_length = list_marker(trimmed, marker);
    if (marker_length > 0 && (was_blank || !open || !stack.empty() || indent < 4)) {
      close();
      while (!stack.empty() && indent < stack.back().content)
        stack.pop_back();
      Block block;
      block.kind   = BlockKind::ListItem;
      block.level  = static_cast< int >(stack.size());
      block.marker = marker == "-" ? (block.level == 0 ? "•" : block.level == 1 ? "◦" : "▪") : marker;
      stack.push_back({ indent, indent + marker_length });
      blocks.push_back(block);
      open = true;
      raw  = trimmed.substr(marker_length);
      continue;
    }
    if (open && !was_blank) {
      append(trimmed);
      continue;
    }
    close();
    Block block;
    if (was_blank && inside_item) {
      // paragraphe de suite d'un element de liste : meme retrait, sans marqueur
      block.kind  = BlockKind::ListItem;
      block.level = static_cast< int >(stack.size()) - 1;
    } else {
      stack.clear();
      block.kind = BlockKind::Paragraph;
    }
    blocks.push_back(block);
    open = true;
    raw  = trimmed;
  }
  close();
  return blocks;
}

void collect_links(const std::vector< Span > &spans, std::vector< std::string > &links) {
  for (const Span &span : spans)
    if (!span.link.empty() && (links.empty() || links.back() != span.link))
      links.push_back(span.link);
}

void collect_links(const std::vector< Block > &blocks, std::vector< std::string > &links) {
  for (const Block &block : blocks) {
    collect_links(block.spans, links);
    collect_links(block.children, links);
    for (const auto &cell : block.header)
      collect_links(cell, links);
    for (const auto &row : block.rows)
      for (const auto &cell : row)
        collect_links(cell, links);
  }
}

std::string percent_decode(const std::string &text) {
  std::string output;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size() && std::isxdigit(static_cast< unsigned char >(text[index + 1]))
        && std::isxdigit(static_cast< unsigned char >(text[index + 2]))) {
      output += static_cast< char >(std::stoi(text.substr(index + 1, 2), nullptr, 16));
      index += 2;
    } else {
      output += text[index];
    }
  }
  return output;
}

std::string dirname(const std::string &path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string basename(const std::string &path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool is_readme(const std::string &path) {
  return basename(path) == "README.md";
}

} // namespace

std::vector< Span > parse_inline(const std::string &text) {
  std::vector< Span > spans;
  parse_into(text, Span{}, spans);
  return spans;
}

std::string plain_text(const std::vector< Span > &spans) {
  std::string text;
  for (const Span &span : spans)
    text += span.text == "\n" ? " " : span.text;
  return text;
}

std::vector< Block > parse(const std::string &source) {
  std::vector< std::string > lines;
  std::size_t                start = 0;
  while (start <= source.size()) {
    std::size_t end = source.find('\n', start);
    if (end == std::string::npos)
      end = source.size();
    std::string line = source.substr(start, end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(line);
    start = end + 1;
  }
  std::vector< Block >          blocks = parse_lines(lines);
  std::map< std::string, int >  seen;
  for (Block &block : blocks) {
    if (block.kind != BlockKind::Heading)
      continue;
    const std::string base  = slug(plain_text(block.spans));
    const int         count = seen[base]++;
    block.anchor            = count == 0 ? base : base + "-" + std::to_string(count);
  }
  return blocks;
}

std::string slug(const std::string &heading) {
  std::string output;
  for (std::size_t index = 0; index < heading.size();) {
    const unsigned char byte = static_cast< unsigned char >(heading[index]);
    if (byte < 0x80) {
      if (std::isalnum(byte) || byte == '-' || byte == '_')
        output += static_cast< char >(std::tolower(byte));
      else if (byte == ' ')
        output += '-';
      ++index;
      continue;
    }
    // caractere UTF-8 : lettres latines gardees (minuscules pour les majuscules accentuees), le reste (guillemets,
    // apostrophe typographique, tirets longs, emojis) retire comme sur GitHub
    std::size_t length = (byte & 0xE0) == 0xC0 ? 2 : (byte & 0xF0) == 0xE0 ? 3 : (byte & 0xF8) == 0xF0 ? 4 : 1;
    length             = std::min(length, heading.size() - index);
    std::uint32_t code = length == 2 ? (byte & 0x1Fu) : length == 3 ? (byte & 0x0Fu) : (byte & 0x07u);
    for (std::size_t next = 1; next < length; ++next)
      code = (code << 6) | (static_cast< unsigned char >(heading[index + next]) & 0x3Fu);
    const bool latin_letter = (code >= 0xC0 && code <= 0x24F && code != 0xD7 && code != 0xF7);
    if (latin_letter) {
      // Latin-1 : majuscules C0-DE -> +0x20 ; Latin etendu A (0x100-0x17F) : paires majuscule/minuscule
      if (code >= 0xC0 && code <= 0xDE)
        code += 0x20;
      else if (code == 0x152)
        code = 0x153; // Œ -> œ
      if (code < 0x800) {
        output += static_cast< char >(0xC0 | (code >> 6));
        output += static_cast< char >(0x80 | (code & 0x3F));
      }
    }
    index += length;
  }
  return output;
}

LinkTarget resolve_link(const std::string &from, const std::string &link) {
  LinkTarget target;
  if (link.find("://") != std::string::npos || starts_with(link, "mailto:")) {
    target.kind = LinkTarget::External;
    target.path = link;
    return target;
  }
  const std::size_t hash = link.find('#');
  const std::string path = percent_decode(link.substr(0, hash));
  if (hash != std::string::npos)
    target.anchor = percent_decode(link.substr(hash + 1));
  if (path.empty()) {
    target.kind = LinkTarget::Anchor;
    return target;
  }
  // chemin dans le depot : docs/<dossier de la page>/<lien>
  std::vector< std::string > segments{ "docs" };
  if (path[0] != '/') {
    const std::string base = dirname(from);
    std::size_t       start = 0;
    while (!base.empty() && start <= base.size()) {
      const std::size_t slash = std::min(base.find('/', start), base.size());
      segments.push_back(base.substr(start, slash - start));
      start = slash + 1;
    }
  } else {
    segments.clear(); // /chemin : depuis la racine du depot
  }
  bool        escaped = false;
  std::size_t start   = 0;
  while (start <= path.size()) {
    const std::size_t slash   = std::min(path.find('/', start), path.size());
    const std::string segment = path.substr(start, slash - start);
    start                     = slash + 1;
    if (segment.empty() || segment == ".")
      continue;
    if (segment == "..") {
      if (segments.empty())
        escaped = true;
      else
        segments.pop_back();
      continue;
    }
    segments.push_back(segment);
  }
  std::string joined;
  for (const std::string &segment : segments)
    joined += (joined.empty() ? "" : "/") + segment;
  if (path.back() == '/')
    joined += (joined.empty() ? "" : "/") + std::string("README.md");
  if (!escaped && starts_with(joined, "docs/")) {
    target.kind = LinkTarget::Page;
    target.path = joined.substr(5);
  } else {
    target.kind = LinkTarget::Outside;
    target.path = escaped ? path : joined;
  }
  return target;
}

Library::Library(const std::map< std::string, std::string > &files) {
  for (const auto &[path, source] : files) {
    Page page;
    page.path   = path;
    page.source = source;
    page.title  = basename(path);
    for (const Block &block : parse(source))
      if (block.kind == BlockKind::Heading && block.level == 1) {
        page.title = plain_text(block.spans);
        break;
      }
    index_[path] = pages_.size();
    pages_.push_back(std::move(page));
  }
  // arborescence des dossiers
  std::map< std::string, Folder > folders; // chemin -> dossier (sans ses sous-dossiers)
  std::map< std::string, std::vector< std::string > > children;
  folders[""].path = "";
  for (const Page &page : pages_) {
    std::string dir = dirname(page.path);
    Folder     &folder = folders[dir];
    folder.path        = dir;
    if (is_readme(page.path))
      folder.readme = page.path;
    else
      folder.pages.push_back(page.path);
    // dossiers parents
    while (!dir.empty()) {
      const std::string parent = dirname(dir);
      folders[parent].path     = parent;
      std::vector< std::string > &list = children[parent];
      if (std::find(list.begin(), list.end(), dir) == list.end())
        list.push_back(dir);
      folders[dir].path = dir;
      dir               = parent;
    }
  }
  // ordre des liens du README de chaque dossier
  const auto link_order = [&](const Folder &folder) {
    std::vector< std::string > order;
    if (folder.readme.empty())
      return order;
    std::vector< std::string > links;
    collect_links(parse(pages_[index_[folder.readme]].source), links);
    for (const std::string &link : links) {
      const LinkTarget target = resolve_link(folder.readme, link);
      if (target.kind == LinkTarget::Page)
        order.push_back(target.path);
    }
    return order;
  };
  const auto rank = [](const std::vector< std::string > &order, const std::string &prefix, bool folder) {
    for (std::size_t index = 0; index < order.size(); ++index)
      if (folder ? starts_with(order[index], prefix + "/") : order[index] == prefix)
        return index;
    return order.size();
  };
  const auto build = [&](const auto &self, const std::string &path) -> Folder {
    Folder folder = folders[path];
    folder.title  = path.empty() ? "Documentation" : basename(path);
    if (!folder.readme.empty())
      folder.title = pages_[index_[folder.readme]].title;
    const std::vector< std::string > order = link_order(folder);
    std::stable_sort(folder.pages.begin(), folder.pages.end(), [&](const std::string &left, const std::string &right) {
      const std::size_t left_rank = rank(order, left, false), right_rank = rank(order, right, false);
      return left_rank != right_rank ? left_rank < right_rank : left < right;
    });
    std::vector< std::string > subfolders = children[path];
    std::sort(subfolders.begin(), subfolders.end(), [&](const std::string &left, const std::string &right) {
      const std::size_t left_rank = rank(order, left, true), right_rank = rank(order, right, true);
      return left_rank != right_rank ? left_rank < right_rank : left < right;
    });
    for (const std::string &subfolder : subfolders)
      folder.folders.push_back(self(self, subfolder));
    return folder;
  };
  root_ = build(build, "");
}

const Page *Library::page(const std::string &path) const {
  const auto found = index_.find(path);
  return found == index_.end() ? nullptr : &pages_[found->second];
}

} // namespace qrprotec::markdown
