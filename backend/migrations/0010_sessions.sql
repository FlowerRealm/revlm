DROP TABLE IF EXISTS session_bindings;
CREATE TABLE IF NOT EXISTS sessions (
  token_hash VARCHAR(64) NOT NULL,
  user_id BIGINT NOT NULL,
  expires_at DATETIME NOT NULL,
  PRIMARY KEY (token_hash),
  KEY sessions_user_id (user_id),
  KEY sessions_expires_at (expires_at)
);
