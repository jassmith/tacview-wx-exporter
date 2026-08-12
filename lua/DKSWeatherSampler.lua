-- DKS Tacview weather exporter — wind sampler.
--
-- Install to: Saved Games/<DCS>/Scripts/Hooks/DKSWeatherSampler.lua
--
-- Runs in the GameGUI environment and periodically asks the mission
-- environment for the wind field, writing an altitude ladder to a small text
-- file that the proxy DLL polls. Kept in Lua deliberately: this is the half
-- that talks to the sim, and it should be readable by anyone deciding whether
-- to trust the exporter on their server.
--
-- The wind is read from atmosphere.getWind() — the value the sim is actually
-- applying — rather than the mission's static weather table, so it stays
-- correct under dynamic weather and under mods that rewrite conditions at
-- runtime. getWind (not getWindWithTurbulence) is deliberate: we want the
-- steady field, not a gust sample.

local SAMPLE_INTERVAL_SEC = 30   -- model-time seconds between wind refreshes

-- Events drain on their OWN, much faster timer.
--
-- Wind can lag half a minute and nobody can tell. An event cannot: the
-- injector places it at its true frame, but only if it learns about it before
-- that frame has been flushed to disk. Draining on the 30 s wind timer put
-- kills 22-84 s late in a real recording, because the frames they belonged to
-- were already written. At 0.5 s the event is known long before Tacview's next
-- ~7 s flush, so the placement logic can actually do its job.
--
-- The cost is one dostring round-trip per tick, measured at under 1 ms on a
-- 2,238-unit mission, and it does not grow with event volume — the handler
-- batches on the mission side.
local EVENT_PUMP_INTERVAL_SEC = 0.5
local MAX_ALTITUDE_M = 12000
local ALTITUDE_STEP_M = 500

-- Model-time seconds to let the mission settle before the first sample.
--
-- This is not politeness, it is a hard requirement. Running code in the
-- mission state while DCS is still building it crashes the server outright:
-- ACCESS_VIOLATION in Terrain::createGlobalLand, reached from
-- Scripting::regLuaAirbase. onSimulationFrame starts firing during that
-- window, so arming on the frame callback alone is not enough — we wait for
-- onMissionLoadEnd AND for the clock to have moved.
local START_DELAY_SEC = 10

local profilePath = lfs.writedir() .. [[Mods\tech\Tacview\bin\dks-wx.profile]]
local eventsPath = lfs.writedir() .. [[Mods\tech\Tacview\bin\dks-wx.events]]
local nextSampleAt = 0
local armed = false
local ready = false

-- Named logMsg, not log: a local `log` would shadow the global log table this
-- very function needs.
local function logMsg(msg)
	log.write('DKS.WX', log.INFO, msg)
end

-- Runs inside the mission environment. DCS wind is horizontally uniform, so a
-- single column reproduces the field; y is altitude MSL.
local function payload()
	return string.format([[
		local out = {}
		for alt = 0, %d, %d do
			local ok, w = pcall(atmosphere.getWind, { x = 0, y = alt, z = 0 })
			if ok and w then
				out[#out+1] = string.format('w %%d %%.4f %%.4f %%.4f', alt, w.x, w.y, w.z)
			end
		end
		return table.concat(out, '\n')
	]], MAX_ALTITUDE_M, ALTITUDE_STEP_M)
end

-- QNH lives in the mission table and does not change during a mission, so it
-- is read once from the GUI side. DCS stores it in mmHg; Tacview reports hPa.
local function qnhLine()
	local ok, mission = pcall(function() return DCS.getCurrentMission().mission end)
	if not ok or not mission or not mission.weather then return nil end
	local mmhg = mission.weather.qnh
	if type(mmhg) ~= 'number' or mmhg <= 0 then return nil end
	return string.format('qnh %.2f', mmhg * 1.33322387415)
end

local function writeProfile(body)
	-- Write-and-rename so the DLL never reads a half-written ladder.
	local tmp = profilePath .. '.tmp'
	local f = io.open(tmp, 'wb')
	if not f then return end
	f:write(body)
	f:close()
	os.remove(profilePath)
	os.rename(tmp, profilePath)
end

local function sample()
	-- 'server' is the state that hosts the mission scripting API (land, env,
	-- atmosphere). 'mission' does not have it — asking there returns the string
	-- "attempt to index global 'atmosphere' (a nil value)".
	local ok, result = pcall(net.dostring_in, 'server', payload())
	if not ok or type(result) ~= 'string' or result == '' then
		logMsg('wind sample failed: ' .. tostring(result))
		return
	end

	-- dostring_in reports script errors by RETURNING the message rather than
	-- raising, so a successful pcall proves nothing. Require real data.
	if not result:find('^w ') then
		logMsg('wind sample returned no data: ' .. result:sub(1, 200))
		return
	end

	local parts = {}
	local qnh = qnhLine()
	if qnh then parts[#parts+1] = qnh end
	parts[#parts+1] = result
	writeProfile(table.concat(parts, '\n') .. '\n')
end

-- Cheap readiness probe. If the scripting environment is not fully up, this
-- returns something other than 'table' rather than us diving straight into a
-- getWind loop against a half-built world.
local function scriptingReady()
	local ok, result = pcall(net.dostring_in, 'server', 'return type(atmosphere)')
	return ok and result == 'table'
end

-- ---------------------------------------------------------------------------
-- Event capture
-- ---------------------------------------------------------------------------
--
-- Kills, deaths and weight on/off wheels are all available as DCS events, so
-- nothing here polls unit state — that is what would have scaled badly on a
-- big mission. The handler appends to a table in the mission state and the GUI
-- side drains it on its own fast timer, which makes the cross-state cost one
-- round-trip per tick regardless of how many events fired.

local EVENT_INSTALL = [[
	if not _DKSWX_EV then
		_DKSWX_EV = {}
		-- Only events confirmed to reach a dedicated server's mission
		-- environment. Notably absent: PLAYER_ENTER_UNIT, which does not fire
		-- server-side at all (the server sees BIRTH when a client takes a
		-- slot), and TAKEOFF/LAND, whose replacements are the runway pair —
		-- TAKEOFF fires "several seconds after take-off" and LAND only once
		-- the aircraft has slowed, so both would timestamp wrongly.
		local wanted = {
			[world.event.S_EVENT_KILL] = 'Kill',
			[world.event.S_EVENT_DEAD] = 'Dead',
			[world.event.S_EVENT_CRASH] = 'Crash',
			[world.event.S_EVENT_UNIT_LOST] = 'UnitLost',
			[world.event.S_EVENT_EJECTION] = 'Ejection',
			[world.event.S_EVENT_PILOT_DEAD] = 'PilotDead',
			[world.event.S_EVENT_RUNWAY_TAKEOFF] = 'RunwayTakeoff',
			[world.event.S_EVENT_RUNWAY_TOUCH] = 'RunwayTouch',
			-- The classic pair as a fallback. They are late (TAKEOFF fires
			-- several seconds after the wheels leave; LAND only once the
			-- aircraft has slowed) so the runway pair is preferred, but a
			-- carrier trap produced RUNWAY_TAKEOFF and no RUNWAY_TOUCH on a
			-- live server, so relying on the runway pair alone loses landings
			-- entirely. The injector collapses whichever arrives second.
			[world.event.S_EVENT_TAKEOFF] = 'RunwayTakeoff',
			[world.event.S_EVENT_LAND] = 'RunwayTouch',
			[world.event.S_EVENT_LANDING_QUALITY_MARK] = 'LSO',
			[world.event.S_EVENT_SHOT] = 'Shot',
			[world.event.S_EVENT_HIT] = 'Hit',
			[world.event.S_EVENT_BDA] = 'BDA',
			[world.event.S_EVENT_BIRTH] = 'Birth',
			[world.event.S_EVENT_REFUELING] = 'Refuel',
			[world.event.S_EVENT_REFUELING_STOP] = 'RefuelStop',
		}
		local handler = {}
		function handler:onEvent(e)
			local kind = e and wanted[e.id]
			if not kind then return end
			-- getName() throws on objects already torn down; never let a
			-- recorder concern break the mission.
			local ok1, who = pcall(function() return e.initiator:getName() end)
			local ok2, tgt = pcall(function() return e.target:getName() end)
			local ok3, place = pcall(function() return e.place:getName() end)
			local ok4, wep = pcall(function() return e.weapon:getTypeName() end)
			-- Tacview writes the PLAYER name in Pilot= for human aircraft and
			-- the UNIT name for AI, while DCS events always name the unit. Send
			-- both so the injector can resolve either; without the player name
			-- every human event is unresolvable and gets dropped.
			-- Both sides: a Kill names the victim as its subject, so the
			-- initiator's player name would be the wrong person entirely.
			local ok5, iname = pcall(function() return e.initiator:getPlayerName() end)
			local ok6, tname = pcall(function() return e.target:getPlayerName() end)
			local playerIni = (ok5 and type(iname) == 'string' and iname ~= '') and iname or ''
			local playerTgt = (ok6 and type(tname) == 'string' and tname ~= '') and tname or ''
			-- The LSO grade arrives as free text on the event itself.
			local note = (type(e.comment) == 'string' and e.comment)
				or (ok4 and wep) or (ok3 and place) or ''
			note = tostring(note):gsub('[|\n\r]', ' ')
			-- Player names are free text and routinely contain '|'
			-- ("FIWB | Miyagi | 400"), so they must stay in trailing fields
			-- that are captured greedily rather than split.
			_DKSWX_EV[#_DKSWX_EV + 1] = string.format('%s|%.2f|%s|%s|%s|%s|%s', kind,
				timer.getTime(), ok1 and who or '', ok2 and tgt or '', note,
				playerIni, playerTgt)
		end
		world.addEventHandler(handler)
	end
	return 'installed'
]]

local EVENT_DRAIN = [[
	if not _DKSWX_EV then return '' end
	local out = table.concat(_DKSWX_EV, '\n')
	_DKSWX_EV = {}
	return out
]]

local eventsInstalled = false
local eventTotal = 0
local nextEventPumpAt = 0

local function pumpEvents()
	if not eventsInstalled then
		local ok = pcall(net.dostring_in, 'server', EVENT_INSTALL)
		if not ok then return end
		eventsInstalled = true
		logMsg('event handler installed')
		return
	end

	local ok, batch = pcall(net.dostring_in, 'server', EVENT_DRAIN)
	if not ok or type(batch) ~= 'string' or batch == '' then return end

	local n = 0
	for _ in batch:gmatch('[^\n]+') do n = n + 1 end
	eventTotal = eventTotal + n
	if batch ~= '' then
		-- Births arrive in waves: one per unit at mission start (thousands on a
		-- large mission) and again whenever a late-activated group spawns. The
		-- start-up flood is already avoided by installing the handler after
		-- START_DELAY_SEC, but a scripted wave could still swamp the stream, so
		-- a batch carrying more than this many is dropped wholesale. The case
		-- worth keeping is the single birth of a human taking a slot, which is
		-- how a dedicated server reports that at all — PLAYER_ENTER_UNIT never
		-- fires here. Every unit is declared as an object by Tacview regardless,
		-- so a dropped wave costs no information the recording lacks.
		local BIRTH_BATCH_CAP = 8
		local births = 0
		for line in batch:gmatch('[^\n]+') do
			if line:sub(1, 6) == 'Birth|' then births = births + 1 end
		end
		local skipBirths = births > BIRTH_BATCH_CAP
		if skipBirths then
			logMsg(string.format('dropping %d Birth events in one batch (cap %d)',
				births, BIRTH_BATCH_CAP))
		end

		local out = {}
		for line in batch:gmatch('[^\n]+') do
			local kind, t, who, tgt, note, playerIni, playerTgt =
				line:match('^([^|]*)|([^|]*)|([^|]*)|([^|]*)|([^|]*)|([^|]*)|?(.*)$')
			if kind then
				-- Map onto Tacview's own vocabulary where one exists, so other
				-- readers understand it without knowing anything about us.
				-- Everything else rides as Message with a DKS: prefix, which
				-- Tacview displays harmlessly and DKS can pick out. Inventing
				-- new event TYPE names risks other tools calling the file
				-- malformed, so the prefix goes in the text instead.
				--
				-- Fields: acmiKind | primary (the subject) | secondary | text
				local k, primary, secondary, text
				if kind == 'Kill' then
					k, primary, secondary, text = 'Destroyed', tgt, who, ''
				elseif kind == 'Dead' or kind == 'UnitLost' or kind == 'Crash' then
					k, primary, secondary, text = 'Destroyed', who, '', ''
				elseif kind == 'RunwayTakeoff' then
					k, primary, secondary, text = 'TakenOff', who, '', note
				elseif kind == 'RunwayTouch' then
					k, primary, secondary, text = 'Landed', who, '', note
				elseif kind == 'LSO' then
					-- Worth seeing on the timeline in Tacview too, so Bookmark
					-- rather than Message. note is the engine's grade string,
					-- e.g. "LSO: GRADE:_OK_ : WIRE# 3".
					k, primary, secondary, text = 'Bookmark', who, '', note
				elseif kind == 'Shot' then
					k, primary, secondary, text = 'Message', who, '', 'DKS:Shot ' .. note
				elseif kind == 'Hit' then
					k, primary, secondary, text = 'Message', tgt, who, 'DKS:Hit ' .. note
				elseif kind == 'BDA' then
					k, primary, secondary, text = 'Message', tgt ~= '' and tgt or who, who, 'DKS:BDA'
				elseif kind == 'Birth' and not skipBirths then
					k, primary, secondary, text = 'Message', who, '', 'DKS:Birth ' .. note
				elseif kind == 'Refuel' then
					k, primary, secondary, text = 'Message', who, '', 'DKS:Refuel'
				elseif kind == 'RefuelStop' then
					k, primary, secondary, text = 'Message', who, '', 'DKS:RefuelStop'
				elseif kind == 'Ejection' then
					k, primary, secondary, text = 'Message', who, '', 'DKS:Ejection'
				elseif kind == 'PilotDead' then
					k, primary, secondary, text = 'Message', who, '', 'DKS:PilotDead'
				end
				if k and primary and primary ~= '' then
					-- Field 3 is the model time at drain: with the event's own
					-- time it shows how long the sampler sat on it.
					-- Alternate key for whichever unit ended up as the primary
					-- subject. A Kill's subject is the target, so it needs the
					-- target's player name; everything else uses the
					-- initiator's. Without this a human shot down by a weapon
					-- stays unresolvable, which is the most common case there
					-- is.
					local alt = ''
					if primary == tgt and playerTgt ~= '' then
						alt = playerTgt
					elseif primary == who and playerIni ~= '' then
						alt = playerIni
					end
					out[#out + 1] = string.format('%s|%s|%.2f|%s|%s|%s|%s', k, t,
						DCS.getModelTime(), primary, secondary or '', text or '', alt)
				end
			end
		end
		if #out > 0 then
			-- Append-only; the DLL reads forward from its own offset, so
			-- neither side needs a lock.
			local f = io.open(eventsPath, 'ab')
			if f then f:write(table.concat(out, '\n') .. '\n') f:close() end
		end
	end
	logMsg(string.format('events: +%d (total %d)', n, eventTotal))
end

local callbacks = {}

function callbacks.onMissionLoadEnd()
	-- Start each mission with an empty event file.
	--
	-- The DLL reads this append-only and tracks its own byte offset, resetting
	-- that offset whenever a new recording opens. On a server rotating
	-- missions, a file carrying the previous mission's events would therefore
	-- be replayed in full into the new recording — and where unit names repeat
	-- across mission templates, which is normal, those stale kills would be
	-- injected against whichever aircraft now holds the name. The previous
	-- mission's file is kept alongside as .prev so the audit trail survives one
	-- rotation.
	os.remove(eventsPath .. '.prev')
	os.rename(eventsPath, eventsPath .. '.prev')
	os.remove(eventsPath)

	armed = true
	ready = false
	nextSampleAt = START_DELAY_SEC
	-- The event pump must respect the same delay. Decoupling it onto its own
	-- timer without this let it call into the mission state at t=1.2, which is
	-- exactly the early-call that crashes the server in
	-- Terrain::createGlobalLand. It also, incidentally, swallowed the flood of
	-- mission-start BIRTH events — one per unit, thousands on a large mission,
	-- none of which belong in the recording since Tacview declares every one
	-- of those objects anyway.
	nextEventPumpAt = START_DELAY_SEC
end

function callbacks.onSimulationFrame()
	if not armed then return end
	local t = DCS.getModelTime()
	if t < nextEventPumpAt and t < nextSampleAt then return end

	if not ready then
		if not scriptingReady() then
			nextSampleAt = t + 5
			return
		end
		ready = true
	end

	if t >= nextEventPumpAt then
		nextEventPumpAt = t + EVENT_PUMP_INTERVAL_SEC
		pcall(pumpEvents)
	end

	if t < nextSampleAt then return end
	nextSampleAt = t + SAMPLE_INTERVAL_SEC
	pcall(sample)
end

function callbacks.onSimulationStop()
	armed = false
	ready = false
	eventsInstalled = false
	nextEventPumpAt = 0
	os.remove(profilePath)
	-- The event file is left in place for post-run auditing; the next
	-- onMissionLoadEnd rotates it to .prev before the new mission writes.
end

DCS.setUserCallbacks(callbacks)
logMsg('weather sampler loaded')
