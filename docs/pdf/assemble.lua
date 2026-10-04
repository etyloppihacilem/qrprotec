-- Assemble plusieurs pages Markdown de docs/ en un seul document pour l'impression (make doc).
--
-- La liste des pages vient de la métadonnée « pages » (chemins relatifs à la racine du dépôt).
-- Chaque page est enveloppée dans un bloc dont l'identifiant dérive de son chemin, et les
-- identifiants de ses titres sont préfixés de la même façon : deux pages peuvent ainsi avoir un
-- titre identique. Les liens entre pages (« notions.md#scellé ») deviennent des liens internes
-- au PDF ; les liens vers un fichier qui n'est pas dans le PDF sont remplacés par leur texte.

local function normalize(path)
  local parts = {}
  for part in path:gmatch('[^/]+') do
    if part == '..' then
      table.remove(parts)
    elseif part ~= '.' then
      table.insert(parts, part)
    end
  end
  return table.concat(parts, '/')
end

local function dirname(path)
  return path:match('^(.*)/[^/]*$') or '.'
end

local function page_id(path)
  return 'p-' .. path:gsub('%.md$', ''):gsub('[^%w]', '-')
end

local function decode(s)
  return (s:gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end))
end

function Pandoc(doc)
  local pages = {}
  local included = {}
  for _, p in ipairs(doc.meta.pages or {}) do
    local path = normalize(pandoc.utils.stringify(p))
    table.insert(pages, path)
    included[path] = true
  end

  local blocks = pandoc.Blocks({})
  for _, path in ipairs(pages) do
    local f = assert(io.open(path, 'r'))
    local text = f:read('a')
    f:close()
    local page = pandoc.read(text, 'markdown+gfm_auto_identifiers-implicit_figures')
    local prefix = page_id(path)
    local dir = dirname(path)

    page = page:walk({
      Header = function(h)
        if h.identifier ~= '' then
          h.identifier = prefix .. '--' .. h.identifier
        end
        return h
      end,
      Link = function(link)
        local target = link.target
        if target:match('^%a[%w+.-]*:') then
          return link
        end
        local file, anchor = target:match('^([^#]*)#?(.*)$')
        anchor = decode(anchor)
        local dest = prefix
        if file ~= '' then
          local resolved = normalize(dir .. '/' .. decode(file))
          if not included[resolved] then
            return link.content
          end
          dest = page_id(resolved)
        end
        link.target = '#' .. dest .. (anchor ~= '' and ('--' .. anchor) or '')
        return link
      end,
    })

    blocks:insert(pandoc.Div(page.blocks, pandoc.Attr(prefix, {'page'})))
  end

  doc.blocks = blocks
  return doc
end
