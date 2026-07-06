import type { Ref } from 'react';

import type { AdminUsageChannelSuggest, AdminUsageUserSuggest } from '../../../api/admin/usage';
import { ChannelSuggestInput } from '../../../components/ChannelSuggestInput';
import { ModelSuggestInput } from '../../../components/ModelSuggestInput';
import { UserSuggestInput } from '../../../components/UserSuggestInput';
import {
  UsageAdvancedFiltersDropdown,
  type UsageAdvancedFiltersDropdownHandle,
} from '../../../components/UsageAdvancedFiltersDropdown';

type Props = {
  advRef: Ref<UsageAdvancedFiltersDropdownHandle>;
  disabled: boolean;
  filterUser: string;
  filterChannel: string;
  filterModel: string;
  allTimeActive: boolean;
  startValue?: string;
  endValue?: string;
  onUserChange: (value: string) => void;
  onUserSelect: (user: AdminUsageUserSuggest) => void;
  onChannelChange: (value: string) => void;
  onChannelSelect: (channel: AdminUsageChannelSuggest) => void;
  onModelChange: (value: string) => void;
  onModelSelect: (model: string) => void;
};

export function UsageAdminAdvancedFilters({
  advRef,
  disabled,
  filterUser,
  filterChannel,
  filterModel,
  allTimeActive,
  startValue,
  endValue,
  onUserChange,
  onUserSelect,
  onChannelChange,
  onChannelSelect,
  onModelChange,
  onModelSelect,
}: Props) {
  return (
    <UsageAdvancedFiltersDropdown
      ref={advRef}
      disabled={disabled}
      toggleTestId="admin-usage-adv-toggle"
      fields={[
        {
          inputId: 'adminUsageFilterUserValue',
          label: '用户',
          title: '用户名/邮箱',
          placeholder: '输入用户名或邮箱',
          value: filterUser,
          onChange: onUserChange,
          render: ({ id, value, onChange, placeholder, disabled: inputDisabled }) => (
            <UserSuggestInput
              id={id}
              value={value}
              placeholder={placeholder}
              disabled={inputDisabled}
              onChange={onChange}
              onSelect={onUserSelect}
            />
          ),
        },
        {
          inputId: 'adminUsageFilterChannelValue',
          label: '渠道',
          title: '渠道(ID/名称)',
          placeholder: '输入渠道 ID 或名称',
          value: filterChannel,
          onChange: onChannelChange,
          render: ({ id, value, onChange, placeholder, disabled: inputDisabled }) => (
            <ChannelSuggestInput
              id={id}
              value={value}
              placeholder={placeholder}
              disabled={inputDisabled}
              start={allTimeActive ? undefined : startValue}
              end={allTimeActive ? undefined : endValue}
              allTime={allTimeActive}
              onChange={onChange}
              onSelect={onChannelSelect}
            />
          ),
        },
        {
          inputId: 'adminUsageFilterModelValue',
          label: '模型',
          title: '模型',
          placeholder: '输入模型名',
          value: filterModel,
          onChange: onModelChange,
          render: ({ id, value, onChange, placeholder, disabled: inputDisabled }) => (
            <ModelSuggestInput
              id={id}
              value={value}
              placeholder={placeholder}
              disabled={inputDisabled}
              start={allTimeActive ? undefined : startValue}
              end={allTimeActive ? undefined : endValue}
              allTime={allTimeActive}
              onChange={onChange}
              onSelect={onModelSelect}
            />
          ),
        },
      ]}
    />
  );
}
