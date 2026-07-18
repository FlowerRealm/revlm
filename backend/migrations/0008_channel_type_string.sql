ALTER TABLE channels MODIFY COLUMN type VARCHAR(64) NOT NULL DEFAULT '';
UPDATE channels SET type = CASE type
  WHEN '4' THEN 'anthropic'
  WHEN '1' THEN 'openai_compatible'
  WHEN '2' THEN 'openai_compatible'
  WHEN 'anthropic' THEN 'anthropic'
  WHEN 'openai_compatible' THEN 'openai_compatible'
  ELSE 'openai_compatible'
END;
