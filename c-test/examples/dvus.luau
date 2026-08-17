-- @desc
-- DVUS — a two-block falling-piece puzzle (domino Tetris).
-- Arrows move · Up/Z/X rotate · Down soft-drop · Space hard-drop · P pause · Esc quit.
-- Faithful port of imbecile-games DVUS (Odinis engine, no audio).
-- @descend

-- Palette from the original Phaser build
local COLORS = {
  "#00f0f0",
  "#f4e409",
  "#28d843",
  "#e13b3b",
  "#2a76dd",
  "#f08a1c",
  "#b84cff",
}

local GRID_W = 10
local GRID_H = 20
local CELL = 14
local BOARD_X = 24
local BOARD_Y = 36
local PREVIEW_X = 198
local PREVIEW_Y = 72
local PREVIEW_SIZE = 56

local BG = "#05070c"
local PANEL = "#10131f"
local GRID_LINE = "#17313b"
local ACCENT = "#50f6c8"
local TITLE_GOLD = "#fff0a8"
local STROKE = "#f8f4c0"
local MUTED = "#8f9bb3"
local DANGER = "#f36f45"

local W, H
local board = {} -- board[y][x] = { node, color } | nil   (1-based)
local active = nil -- { x, y, rotation, color, nodes }
local nextColor
local score = 0
local lines = 0
local level = 1
local dropDelay = 0.76
local dropElapsed = 0
local softDropAcc = 0
local paused = false
local gameOver = false
local state = "menu" -- menu | play
local lockPiece -- forward decl (softDrop locks on fail)

local hudScore
local hudLines
local hudLevel
local hudHint
local titleText
local menuHint
local pauseLabel
local gameOverPanel
local gameOverTitle
local gameOverHint
local previewNodes = {}
local boardBg
local gridLines = {}

local function clamp(v, lo, hi)
  if v < lo then
    return lo
  end
  if v > hi then
    return hi
  end
  return v
end

local function pickColor()
  return COLORS[1 + math.floor(math.random() * #COLORS)]
end

-- Domino: even rotation = horizontal, odd = vertical
local function offsets(rotation)
  if rotation % 2 == 0 then
    return { { 0, 0 }, { 1, 0 } }
  end
  return { { 0, 0 }, { 0, 1 } }
end

local function cellLeft(gx)
  return BOARD_X + (gx - 1) * CELL
end

local function cellTop(gy)
  return BOARD_Y + (gy - 1) * CELL
end

local function slashGeometry(gx, gy, size)
  -- Match Phaser drawBlock slash: diagonal + top highlight
  local x = cellLeft(gx)
  local y = cellTop(gy)
  local len = size * 0.72
  local thick = math.max(2, size * 0.12)
  local cx = x + size * 0.5
  local cy = y + size * 0.5
  return {
    slashX = cx - len * 0.5,
    slashY = cy - thick * 0.5,
    slashW = len,
    slashH = thick,
    hiX = x + size * 0.18,
    hiY = y + size * 0.2,
    hiW = size * 0.42,
    hiH = math.max(1.5, size * 0.08),
  }
end

local function makeBlock(name, gx, gy, color, size)
  size = size or CELL
  local inset = math.max(2, math.floor(size * 0.14))
  local x = cellLeft(gx)
  local y = cellTop(gy)
  local node = document:create("primitive", {
    kind = "rectangle",
    name = name,
    x = x + inset * 0.5,
    y = y + inset * 0.5,
    width = size - inset,
    height = size - inset,
    fill = color,
  })
  node.stroke = { color = STROKE, width = 2, align = "inside" }
  frame:appendChild(node)

  local g = slashGeometry(gx, gy, size)
  local slash = document:create("primitive", {
    kind = "rectangle",
    name = name .. "Slash",
    x = g.slashX,
    y = g.slashY,
    width = g.slashW,
    height = g.slashH,
    fill = "#05070c",
  })
  slash.opacity = 0.55
  slash.rotation = -45
  frame:appendChild(slash)

  local highlight = document:create("primitive", {
    kind = "rectangle",
    name = name .. "Hi",
    x = g.hiX,
    y = g.hiY,
    width = g.hiW,
    height = g.hiH,
    fill = "#ffffff",
  })
  highlight.opacity = 0.4
  frame:appendChild(highlight)

  return { fill = node, slash = slash, highlight = highlight, color = color, size = size }
end

local function placeBlockVisual(block, gx, gy, size)
  size = size or block.size or CELL
  local inset = math.max(2, math.floor(size * 0.14))
  local x = cellLeft(gx)
  local y = cellTop(gy)
  block.fill.x = x + inset * 0.5
  block.fill.y = y + inset * 0.5
  block.fill.width = size - inset
  block.fill.height = size - inset
  if block.slash then
    local g = slashGeometry(gx, gy, size)
    block.slash.x = g.slashX
    block.slash.y = g.slashY
    block.slash.width = g.slashW
    block.slash.height = g.slashH
    block.slash.rotation = -45
    block.highlight.x = g.hiX
    block.highlight.y = g.hiY
    block.highlight.width = g.hiW
    block.highlight.height = g.hiH
  end
end

local function removeBlock(block)
  if block.fill then
    block.fill:remove()
  end
  if block.slash then
    block.slash:remove()
  end
  if block.highlight then
    block.highlight:remove()
  end
end

local function clearPreview()
  for _, block in ipairs(previewNodes) do
    removeBlock(block)
  end
  previewNodes = {}
end

local function drawPreview()
  clearPreview()
  local offs = offsets(0)
  local scale = 18 / CELL
  local size = CELL * scale
  -- Center horizontal domino in the preview box
  local originX = PREVIEW_X + (PREVIEW_SIZE - size * 2) * 0.5
  local originY = PREVIEW_Y + (PREVIEW_SIZE - size) * 0.5
  for i, off in ipairs(offs) do
    local block = document:create("primitive", {
      kind = "rectangle",
      name = "Preview" .. i,
      x = originX + off[1] * size + 1,
      y = originY + off[2] * size + 1,
      width = size - 2,
      height = size - 2,
      fill = nextColor,
    })
    block.stroke = { color = STROKE, width = 1, align = "inside" }
    frame:appendChild(block)
    table.insert(previewNodes, { fill = block, slash = nil, highlight = nil, color = nextColor })
  end
end

local function syncHud()
  hudScore.text = string.format("SCORE\n%06d", score)
  hudLines.text = string.format("LINES\n%03d", lines)
  hudLevel.text = string.format("LEVEL\n%02d", level)
end

local function clearActive()
  if not active then
    return
  end
  for _, block in ipairs(active.nodes) do
    removeBlock(block)
  end
  active = nil
end

local function isValid(gx, gy, rotation)
  for _, off in ipairs(offsets(rotation)) do
    local x = gx + off[1]
    local y = gy + off[2]
    if x < 1 or x > GRID_W or y < 1 or y > GRID_H then
      return false
    end
    if board[y][x] then
      return false
    end
  end
  return true
end

local function syncActiveVisual()
  if not active then
    return
  end
  local offs = offsets(active.rotation)
  for i, off in ipairs(offs) do
    placeBlockVisual(active.nodes[i], active.x + off[1], active.y + off[2])
  end
end

local function spawnPiece()
  local color = nextColor
  nextColor = pickColor()
  drawPreview()
  local startX = 5 -- floor(10/2)-1 in 0-based = 4 → 5 in 1-based
  active = {
    x = startX,
    y = 1,
    rotation = 0,
    color = color,
    nodes = {},
  }
  for i = 1, 2 do
    local block = makeBlock("Active" .. i, startX, 1, color)
    table.insert(active.nodes, block)
  end
  syncActiveVisual()
  if not isValid(active.x, active.y, active.rotation) then
    return false
  end
  dropElapsed = 0
  return true
end

local function tryMove(dx, dy)
  if not active or paused or gameOver then
    return false
  end
  local nx = active.x + dx
  local ny = active.y + dy
  if not isValid(nx, ny, active.rotation) then
    return false
  end
  active.x = nx
  active.y = ny
  syncActiveVisual()
  return true
end

local function rotatePiece(direction)
  if not active or paused or gameOver then
    return
  end
  local nextRot = active.rotation + direction
  if not isValid(active.x, active.y, nextRot) then
    return
  end
  active.rotation = nextRot
  syncActiveVisual()
end

-- Matches original: gravity and Down both soft-drop (+1 score) and lock on fail.
local function softDrop()
  if not active or paused or gameOver then
    return
  end
  if tryMove(0, 1) then
    score += 1
    syncHud()
    return
  end
  lockPiece()
end

local function clearLines()
  local fullRows = {}
  for y = 1, GRID_H do
    local full = true
    for x = 1, GRID_W do
      if not board[y][x] then
        full = false
        break
      end
    end
    if full then
      table.insert(fullRows, y)
    end
  end
  if #fullRows == 0 then
    syncHud()
    return
  end

  local clearSet = {}
  for _, y in ipairs(fullRows) do
    clearSet[y] = true
    for x = 1, GRID_W do
      local cell = board[y][x]
      if cell then
        removeBlock(cell)
        board[y][x] = nil
      end
    end
  end

  local newBoard = {}
  for y = 1, GRID_H do
    newBoard[y] = {}
    for x = 1, GRID_W do
      newBoard[y][x] = nil
    end
  end

  local writeY = GRID_H
  for y = GRID_H, 1, -1 do
    if not clearSet[y] then
      for x = 1, GRID_W do
        newBoard[writeY][x] = board[y][x]
      end
      writeY -= 1
    end
  end
  board = newBoard

  for y = 1, GRID_H do
    for x = 1, GRID_W do
      local cell = board[y][x]
      if cell then
        placeBlockVisual(cell, x, y)
      end
    end
  end

  local lineScores = { 0, 100, 300, 500, 800 }
  lines += #fullRows
  level = math.floor(lines / 10) + 1
  dropDelay = math.max(0.095, 0.76 - (level - 1) * 0.058)
  score += (lineScores[#fullRows] or 800) * level
  syncHud()
end

lockPiece = function()
  if not active then
    return
  end
  local offs = offsets(active.rotation)
  for i, off in ipairs(offs) do
    local x = active.x + off[1]
    local y = active.y + off[2]
    board[y][x] = active.nodes[i]
  end
  active = nil
  clearLines()
  if not spawnPiece() then
    gameOver = true
    paused = false
    if pauseLabel then
      pauseLabel.visible = false
    end
    gameOverPanel.visible = true
    gameOverTitle.visible = true
    gameOverHint.visible = true
    hudHint.text = "Enter restart · Esc quit"
  end
end

local function hardDrop()
  if not active or paused or gameOver then
    return
  end
  local rows = 0
  while tryMove(0, 1) do
    rows += 1
  end
  score += rows * 2
  syncHud()
  lockPiece()
end

local function togglePause()
  if gameOver or state ~= "play" then
    return
  end
  paused = not paused
  if pauseLabel then
    pauseLabel.visible = paused
  end
  if paused then
    hudHint.text = "Paused — P to resume · Esc quit"
  else
    hudHint.text = "Arrows · Up/Z/X rotate · Space drop · P pause"
  end
end

local function resetBoard()
  for y = 1, GRID_H do
    for x = 1, GRID_W do
      local cell = board[y] and board[y][x]
      if cell then
        removeBlock(cell)
      end
    end
  end
  board = {}
  for y = 1, GRID_H do
    board[y] = {}
    for x = 1, GRID_W do
      board[y][x] = nil
    end
  end
  clearActive()
end

local function startPlay()
  state = "play"
  score = 0
  lines = 0
  level = 1
  dropDelay = 0.76
  dropElapsed = 0
  paused = false
  gameOver = false
  titleText.visible = false
  menuHint.visible = false
  gameOverPanel.visible = false
  gameOverTitle.visible = false
  gameOverHint.visible = false
  if pauseLabel then
    pauseLabel.visible = false
  end
  hudHint.text = "Arrows · Up/Z/X rotate · Space drop · P pause"
  resetBoard()
  nextColor = pickColor()
  spawnPiece()
  syncHud()
end

local function showMenu()
  state = "menu"
  paused = false
  gameOver = false
  clearActive()
  resetBoard()
  clearPreview()
  titleText.visible = true
  menuHint.visible = true
  gameOverPanel.visible = false
  gameOverTitle.visible = false
  gameOverHint.visible = false
  if pauseLabel then
    pauseLabel.visible = false
  end
  hudHint.text = "Enter or click to start · Esc quit"
  score = 0
  lines = 0
  level = 1
  syncHud()
end

function init()
  frame.width = 360
  frame.height = 380
  W = frame.width
  H = frame.height

  math.randomseed(math.floor((Mouse.x + 1) * 9973 + (Mouse.y + 1) * 7919) % 2147483647)

  frame.fill = BG
  frame.name = frame.name ~= "" and frame.name or "DVUS"

  -- Outer chrome
  local chrome = document:create("primitive", {
    kind = "rectangle",
    name = "Chrome",
    x = 6,
    y = 6,
    width = W - 12,
    height = H - 12,
    fill = BG,
  })
  chrome.stroke = { color = "#465267", width = 2, align = "inside" }
  frame:appendChild(chrome)

  local titleBar = document:create("primitive", {
    kind = "rectangle",
    name = "TitleBar",
    x = 12,
    y = 10,
    width = W - 24,
    height = 16,
    fill = "#102a37",
  })
  frame:appendChild(titleBar)

  local exe = document:create("text", {
    name = "Exe",
    text = "DVUS.EXE",
    x = 18,
    y = 11,
    fill = ACCENT,
  })
  exe.fontFamily = "Courier New"
  exe.fontSize = 11
  frame:appendChild(exe)

  -- Playfield
  boardBg = document:create("primitive", {
    kind = "rectangle",
    name = "BoardBg",
    x = BOARD_X - 4,
    y = BOARD_Y - 4,
    width = GRID_W * CELL + 8,
    height = GRID_H * CELL + 8,
    fill = PANEL,
  })
  boardBg.stroke = { color = ACCENT, width = 2, align = "outside" }
  frame:appendChild(boardBg)

  for x = 0, GRID_W do
    local line = document:create("primitive", {
      kind = "line",
      name = "GridV" .. x,
      x = BOARD_X + x * CELL,
      y = BOARD_Y,
      width = 0,
      height = GRID_H * CELL,
      fill = GRID_LINE,
    })
    line.opacity = 0.72
    line.strokeWidth = 1
    frame:appendChild(line)
    table.insert(gridLines, line)
  end
  for y = 0, GRID_H do
    local line = document:create("primitive", {
      kind = "line",
      name = "GridH" .. y,
      x = BOARD_X,
      y = BOARD_Y + y * CELL,
      width = GRID_W * CELL,
      height = 0,
      fill = GRID_LINE,
    })
    line.opacity = 0.72
    line.strokeWidth = 1
    frame:appendChild(line)
    table.insert(gridLines, line)
  end

  -- NEXT panel
  local nextLabel = document:create("text", {
    name = "NextLabel",
    text = "NEXT",
    x = PREVIEW_X - 4,
    y = PREVIEW_Y - 28,
    fill = TITLE_GOLD,
  })
  nextLabel.fontFamily = "Courier New"
  nextLabel.fontSize = 16
  frame:appendChild(nextLabel)

  local previewBox = document:create("primitive", {
    kind = "rectangle",
    name = "PreviewBox",
    x = PREVIEW_X,
    y = PREVIEW_Y,
    width = PREVIEW_SIZE,
    height = PREVIEW_SIZE,
    fill = "#07161f",
  })
  previewBox.stroke = { color = "#28d9bc", width = 2, align = "outside" }
  frame:appendChild(previewBox)

  hudScore = document:create("text", {
    name = "HudScore",
    text = "SCORE\n000000",
    x = PREVIEW_X - 4,
    y = PREVIEW_Y + PREVIEW_SIZE + 18,
    fill = ACCENT,
  })
  hudScore.fontFamily = "Courier New"
  hudScore.fontSize = 14
  frame:appendChild(hudScore)

  hudLines = document:create("text", {
    name = "HudLines",
    text = "LINES\n000",
    x = PREVIEW_X - 4,
    y = PREVIEW_Y + PREVIEW_SIZE + 70,
    fill = ACCENT,
  })
  hudLines.fontFamily = "Courier New"
  hudLines.fontSize = 14
  frame:appendChild(hudLines)

  hudLevel = document:create("text", {
    name = "HudLevel",
    text = "LEVEL\n01",
    x = PREVIEW_X - 4,
    y = PREVIEW_Y + PREVIEW_SIZE + 122,
    fill = ACCENT,
  })
  hudLevel.fontFamily = "Courier New"
  hudLevel.fontSize = 14
  frame:appendChild(hudLevel)

  local controls = document:create("text", {
    name = "Controls",
    text = "SPACE DROP\nP PAUSE",
    x = PREVIEW_X - 4,
    y = H - 70,
    fill = MUTED,
  })
  controls.fontFamily = "Courier New"
  controls.fontSize = 11
  frame:appendChild(controls)

  hudHint = document:create("text", {
    name = "HudHint",
    text = "",
    x = 14,
    y = H - 22,
    fill = MUTED,
  })
  hudHint.fontFamily = "Courier New"
  hudHint.fontSize = 10
  frame:appendChild(hudHint)

  titleText = document:create("text", {
    name = "Title",
    text = "DVUS",
    x = BOARD_X + 28,
    y = BOARD_Y + 90,
    fill = TITLE_GOLD,
  })
  titleText.fontFamily = "Courier New"
  titleText.fontSize = 36
  frame:appendChild(titleText)

  menuHint = document:create("text", {
    name = "MenuHint",
    text = "PRESS ENTER",
    x = BOARD_X + 18,
    y = BOARD_Y + 150,
    fill = ACCENT,
  })
  menuHint.fontFamily = "Courier New"
  menuHint.fontSize = 16
  frame:appendChild(menuHint)

  pauseLabel = document:create("text", {
    name = "Pause",
    text = "PAUSED",
    x = BOARD_X + 32,
    y = BOARD_Y + 130,
    fill = TITLE_GOLD,
  })
  pauseLabel.fontFamily = "Courier New"
  pauseLabel.fontSize = 24
  pauseLabel.visible = false
  frame:appendChild(pauseLabel)

  gameOverPanel = document:create("primitive", {
    kind = "rectangle",
    name = "GameOverPanel",
    x = BOARD_X + 8,
    y = BOARD_Y + 90,
    width = GRID_W * CELL - 16,
    height = 90,
    fill = BG,
  })
  gameOverPanel.opacity = 0.92
  gameOverPanel.stroke = { color = DANGER, width = 2, align = "inside" }
  gameOverPanel.visible = false
  frame:appendChild(gameOverPanel)

  gameOverTitle = document:create("text", {
    name = "GameOverTitle",
    text = "GAME OVER",
    x = BOARD_X + 22,
    y = BOARD_Y + 112,
    fill = DANGER,
  })
  gameOverTitle.fontFamily = "Courier New"
  gameOverTitle.fontSize = 18
  gameOverTitle.visible = false
  frame:appendChild(gameOverTitle)

  gameOverHint = document:create("text", {
    name = "GameOverHint",
    text = "PRESS ENTER",
    x = BOARD_X + 28,
    y = BOARD_Y + 148,
    fill = ACCENT,
  })
  gameOverHint.fontFamily = "Courier New"
  gameOverHint.fontSize = 14
  gameOverHint.visible = false
  frame:appendChild(gameOverHint)

  resetBoard()
  showMenu()
end

function update(dt)
  if Keyboard.wasPressed("escape") then
    exit()
    return
  end

  dt = clamp(dt, 0, 0.05)

  if state == "menu" then
    if Keyboard.wasPressed("enter") or Mouse.wasPressed(1) then
      startPlay()
    end
    return
  end

  if gameOver then
    if Keyboard.wasPressed("enter") or Mouse.wasPressed(1) then
      startPlay()
    end
    return
  end

  if Keyboard.wasPressed("p") then
    togglePause()
  end
  if paused then
    return
  end

  if Keyboard.wasPressed("left") or Keyboard.wasPressed("a") then
    tryMove(-1, 0)
  end
  if Keyboard.wasPressed("right") or Keyboard.wasPressed("d") then
    tryMove(1, 0)
  end
  -- Hold Down for continuous soft-drop (wasPressed is edge-only; OS key-repeat isn't exposed)
  if Keyboard.wasPressed("down") or Keyboard.wasPressed("s") then
    softDropAcc = 0
    softDrop()
  elseif Keyboard.isDown("down") or Keyboard.isDown("s") then
    softDropAcc += dt
    if softDropAcc >= 0.05 then
      softDropAcc = 0
      softDrop()
    end
  else
    softDropAcc = 0
  end
  if Keyboard.wasPressed("up") or Keyboard.wasPressed("x") then
    rotatePiece(1)
  end
  if Keyboard.wasPressed("z") then
    rotatePiece(-1)
  end
  if Keyboard.wasPressed("space") then
    hardDrop()
  end

  dropElapsed += dt
  if dropElapsed >= dropDelay then
    dropElapsed = 0
    softDrop()
  end
end

function draw()
end
