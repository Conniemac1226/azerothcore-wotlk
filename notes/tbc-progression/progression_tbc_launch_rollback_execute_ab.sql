-- Progression TBC launch rollback execution (A/B only)
-- WRITE TARGET: acore_characters_progression ONLY
-- NO writes to acore_world_progression, acore_characters, acore_world

SET @run_ts := DATE_FORMAT(UTC_TIMESTAMP(), '%Y%m%d_%H%i%S');
SELECT 'RUN_TS' AS k, @run_ts AS v;

-- =========================================================
-- PHASE A (EXECUTE): Level cap to 70
-- =========================================================
SELECT 'PHASE_A_PRE_COUNT_LEVEL_GT70' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level > 70;

SET @sql := CONCAT(
  'CREATE TABLE acore_characters_progression.backup_characters_lvl_gt70_', @run_ts,
  ' AS SELECT * FROM acore_characters_progression.characters WHERE level > 70'
);
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT 'PHASE_A_BACKUP_SOURCE_COUNT' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level > 70;

UPDATE acore_characters_progression.characters
SET level = 70
WHERE level > 70;

SELECT 'PHASE_A_POST_COUNT_LEVEL_GT70' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.characters
WHERE level > 70;

-- =========================================================
-- PHASE B (EXECUTE): Cap listed skills value/max to 375
-- =========================================================
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
SET value = LEAST(value, 375),
    max   = LEAST(max, 375)
WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773)
  AND (value > 375 OR max > 375);

SELECT 'PHASE_B_POST_COUNT_SKILLS_GT375' AS metric, COUNT(*) AS cnt
FROM acore_characters_progression.character_skills
WHERE skill IN (129,164,165,171,182,185,186,197,202,333,356,393,755,773)
  AND (value > 375 OR max > 375);

-- =========================================================
-- PHASE C/D/E (NOT EXECUTED): kept SELECT-only placeholders
-- =========================================================
SELECT 'PHASE_C_NOT_EXECUTED' AS note;
SELECT 'PHASE_D_NOT_EXECUTED' AS note;
SELECT 'PHASE_E_NOT_EXECUTED' AS note;
