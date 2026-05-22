-- Progression TBC-launch rollback plan (DO NOT RUN BLIND)
-- Target DBs only:
--   acore_characters_progression
--   acore_world_progression (read-only lookups)
--
-- IMPORTANT OPERATIONAL NOTE:
--   Stop worldserver-progression before executing this script.
--   Do not run against live/PTR DBs.

SET @run_ts := DATE_FORMAT(UTC_TIMESTAMP(), '%Y%m%d_%H%i%S');
SELECT 'RUN_TS' AS k, @run_ts AS v;

-- =====================================================================
-- PHASE A: LEVEL CAP (apply)
-- =====================================================================
SELECT 'PHASE_A_PRE_COUNT_LEVEL_GT70' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level > 70;

SET @sql := CONCAT(
  'CREATE TABLE acore_characters_progression.backup_characters_lvl_gt70_', @run_ts,
  ' AS SELECT * FROM acore_characters_progression.characters WHERE level > 70'
);
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT 'PHASE_A_BACKUP_COUNT' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level > 70;

UPDATE acore_characters_progression.characters
SET level = 70
WHERE level > 70;

SELECT 'PHASE_A_POST_COUNT_LEVEL_GT70' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level > 70;

SELECT 'PHASE_A_POST_LEVEL70_COUNT' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level = 70;

-- =====================================================================
-- PHASE B: PROFESSION / SECONDARY CAPS (apply)
-- NOTE: Includes Inscription (773) for now as requested audit scope.
-- =====================================================================
SELECT 'PHASE_B_PRE_COUNT_SKILLS_GT375' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.character_skills
WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773)
  AND (value > 375 OR max > 375);

SET @sql := CONCAT(
  'CREATE TABLE acore_characters_progression.backup_character_skills_gt375_', @run_ts,
  ' AS SELECT * FROM acore_characters_progression.character_skills ',
  'WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773) ',
  'AND (value > 375 OR max > 375)'
);
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

UPDATE acore_characters_progression.character_skills
SET
  value = LEAST(value, 375),
  max   = LEAST(max, 375)
WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773)
  AND (value > 375 OR max > 375);

SELECT 'PHASE_B_POST_COUNT_SKILLS_GT375' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.character_skills
WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773)
  AND (value > 375 OR max > 375);

SELECT 'PHASE_B_POST_BY_SKILL' AS metric, skill, COUNT(*) AS cnt
FROM acore_characters_progression.character_skills
WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773)
  AND (value = 375 OR max = 375)
GROUP BY skill
ORDER BY skill;

-- =====================================================================
-- PHASE C: POST-LAUNCH TBC REPUTATIONS (backup + proposal only; no write)
-- Factions:
-- 1077 Shattered Sun Offensive
-- 1012 Ashtongue Deathsworn
--  990 Scale of the Sands
-- 1015 Netherwing
-- 1031 Sha'tari Skyguard
-- 1038 Ogri'la
-- =====================================================================
SELECT 'PHASE_C_PRE_REP_ROWS' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.character_reputation
WHERE faction IN (1077,1012,990,1015,1031,1038);

SET @sql := CONCAT(
  'CREATE TABLE acore_characters_progression.backup_character_reputation_postlaunch_tbc_', @run_ts,
  ' AS SELECT * FROM acore_characters_progression.character_reputation ',
  'WHERE faction IN (1077,1012,990,1015,1031,1038)'
);
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT 'PHASE_C_REP_DIST' AS metric, faction, COUNT(*) AS cnt, MAX(standing) AS max_standing
FROM acore_characters_progression.character_reputation
WHERE faction IN (1077,1012,990,1015,1031,1038)
GROUP BY faction
ORDER BY faction;

-- Proposal A (safer, reversible with backup restore):
--   UPDATE ... SET standing = 0 WHERE faction IN (...);
-- Proposal B (more destructive):
--   DELETE FROM ... WHERE faction IN (...);
-- Recommendation: use Proposal A first.

-- =====================================================================
-- PHASE D: POST-LAUNCH TBC QUEST COMPLETIONS (backup + proposal only; no write)
-- Curated candidate list from current audit/world lookup:
-- 11481 Crisis at the Sunwell
-- 11488 Magisters' Terrace
-- 11514 Maintaining the Sunwell Portal
-- 11518 Sunwell Daily Portal Flag
-- 11534 Report to Nasuun
-- 11549 A Magnanimous Benefactor
-- =====================================================================
DROP TEMPORARY TABLE IF EXISTS acore_characters_progression.tmp_postlaunch_tbc_quests;
CREATE TEMPORARY TABLE acore_characters_progression.tmp_postlaunch_tbc_quests (
  quest INT UNSIGNED PRIMARY KEY
);

INSERT INTO acore_characters_progression.tmp_postlaunch_tbc_quests (quest) VALUES
(11481),(11488),(11514),(11518),(11534),(11549);

SELECT 'PHASE_D_PRE_QUEST_ROWS' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.character_queststatus_rewarded qr
JOIN acore_characters_progression.tmp_postlaunch_tbc_quests t ON t.quest = qr.quest;

SET @sql := CONCAT(
  'CREATE TABLE acore_characters_progression.backup_character_queststatus_rewarded_postlaunch_tbc_', @run_ts,
  ' AS SELECT qr.* FROM acore_characters_progression.character_queststatus_rewarded qr ',
  'JOIN acore_characters_progression.tmp_postlaunch_tbc_quests t ON t.quest = qr.quest'
);
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT 'PHASE_D_QUEST_DIST' AS metric, qr.quest, COUNT(*) AS cnt
FROM acore_characters_progression.character_queststatus_rewarded qr
JOIN acore_characters_progression.tmp_postlaunch_tbc_quests t ON t.quest = qr.quest
GROUP BY qr.quest
ORDER BY cnt DESC, qr.quest;

-- Proposed write (DO NOT RUN until quest list approved):
-- DELETE qr
-- FROM acore_characters_progression.character_queststatus_rewarded qr
-- JOIN acore_characters_progression.tmp_postlaunch_tbc_quests t ON t.quest = qr.quest;

-- =====================================================================
-- PHASE E: RECIPE/SPELL DENYLIST DISCOVERY (SELECT-only)
-- =====================================================================
-- E1: Candidate learned spells by name pattern (may be sparse / incomplete)
SELECT
  cs.spell,
  sp.Name_Lang_enUS AS spell_name,
  COUNT(*) AS learned_count
FROM acore_characters_progression.character_spell cs
JOIN acore_world_progression.spell_dbc sp ON sp.ID = cs.spell
WHERE sp.Name_Lang_enUS LIKE '%Sunwell%'
   OR sp.Name_Lang_enUS LIKE '%Quel%Danas%'
   OR sp.Name_Lang_enUS LIKE '%Magisters'' Terrace%'
   OR sp.Name_Lang_enUS LIKE '%Zul''Aman%'
   OR sp.Name_Lang_enUS LIKE '%Black Temple%'
   OR sp.Name_Lang_enUS LIKE '%Hyjal%'
   OR sp.Name_Lang_enUS LIKE '%Serpentshrine%'
   OR sp.Name_Lang_enUS LIKE '%Tempest Keep%'
GROUP BY cs.spell, sp.Name_Lang_enUS
ORDER BY learned_count DESC, cs.spell;

-- E2: Candidate craft recipe items gated by post-launch factions (world reference)
SELECT
  it.entry AS item_entry,
  it.name AS item_name,
  it.spellid_1 AS teaches_spell,
  it.RequiredReputationFaction,
  it.RequiredReputationRank,
  it.RequiredSkill,
  it.RequiredSkillRank
FROM acore_world_progression.item_template it
WHERE it.class = 9
  AND it.RequiredReputationFaction IN (1077,1012,990,1015,1031,1038)
ORDER BY it.RequiredReputationFaction, it.RequiredReputationRank, it.entry;

-- E3: Of the above faction-gated taught spells, which are currently learned
SELECT
  it.spellid_1 AS spell_id,
  sp.Name_Lang_enUS AS spell_name,
  it.entry AS source_item_entry,
  it.name AS source_item_name,
  it.RequiredReputationFaction,
  it.RequiredReputationRank,
  COUNT(cs.guid) AS learned_count
FROM acore_world_progression.item_template it
LEFT JOIN acore_world_progression.spell_dbc sp ON sp.ID = it.spellid_1
LEFT JOIN acore_characters_progression.character_spell cs ON cs.spell = it.spellid_1
WHERE it.class = 9
  AND it.RequiredReputationFaction IN (1077,1012,990,1015,1031,1038)
GROUP BY it.spellid_1, sp.Name_Lang_enUS, it.entry, it.name, it.RequiredReputationFaction, it.RequiredReputationRank
ORDER BY learned_count DESC, it.RequiredReputationFaction, it.entry;

-- End of plan.
