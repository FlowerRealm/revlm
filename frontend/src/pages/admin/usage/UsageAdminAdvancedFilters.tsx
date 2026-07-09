import type { Ref } from 'react';

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
  onUserChange: (value: string) => void;
  onChannelChange: (value: string) => void;
  onModelChange: (value: string) => void;
};

export function UsageAdminAdvancedFilters({
  advRef,
  disabled,
  filterUser,
  filterChannel,
  filterModel,
  onUserChange,
  onChannelChange,
  onModelChange,
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
        },
        {
          inputId: 'adminUsageFilterChannelValue',
          label: '渠道',
          title: '渠道(ID/名称)',
          placeholder: '输入渠道 ID 或名称',
          value: filterChannel,
          onChange: onChannelChange,
        },
        {
          inputId: 'adminUsageFilterModelValue',
          label: '模型',
          title: '模型',
          placeholder: '输入模型名',
          value: filterModel,
          onChange: onModelChange,
        },
      ]}
    />
  );
}
