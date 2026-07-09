# Channel group ID references

Issue #400 moved the core channel-group relationships from mutable group names
to `channel_group_id`.

## Current model

`channel_groups.id` is the reference key for these relationship tables:

- `token_channel_groups.channel_group_id`

`channel_groups.name` remains the admin-facing natural key. API request and
response fields can still be named `group_name` or `channel_group_name`, but
store reads derive those display names by joining back to `channel_groups`.

Channel-group rename now updates only `channel_groups.name`, plus the group row
metadata. It does not scan token bindings.

## Migration behavior

`0119_channel_group_id_refs.sql` performs the one-way cleanup:

- adds `token_channel_groups.channel_group_id` when missing;
- on old installations only, also bridges historical `subscription_plans` and
  `managed_models` name references before those tables are dropped by later
  cleanup migrations;
- backfills ids from legacy name columns;
- deletes dangling token bindings;
- historically left blank or dangling managed-model groups as `NULL`; configured
  model tables are later dropped by `0121_drop_configured_models.sql`;
- switches `token_channel_groups` to primary key `(token_id, channel_group_id)`;
- drops the legacy name reference columns.

The remaining name fields are intentionally not relationship keys:

- API JSON fields are input/display surfaces;
- channel group membership already uses `channel_group_members.parent_group_id`.
- Billing multipliers on usage rows are numeric `tier_multiplier` / `channel_multiplier` (no group-name snapshot column).
