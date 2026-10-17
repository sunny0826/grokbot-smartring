'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let session = null, state = null, selected = null, pending = null;
  let timer = null, polling = false, expired = false, offline = false;
  const errors = {
    voice_busy: '设备正在使用语音，请稍后重新打开配网。', scan_failed: '搜索失败，可以重新搜索。',
    no_networks: '没有找到附近网络。请确认路由器已开启。', unsupported: '暂不支持这个网络的认证方式，请选择个人 Wi-Fi。',
    invalid_password: '密码格式不正确，请重新输入。', authentication: '连接未成功，请核对 Wi-Fi 密码后重试。',
    not_found: '没有找到目标网络，请靠近路由器后重试。', timeout: '连接超时，请确认网络可用后重试。',
    storage: 'Wi-Fi 未能保存，请重试。', storage_uncertain: '保存结果尚不确定，请查看圆屏提示。',
    cancelled: '连接已取消，可以重新选择网络。', internal: '设备暂时无法完成设置，请查看圆屏提示。'
  };
  function name(hex) {
    if (typeof hex !== 'string' || !/^(?:[\da-f]{2}){1,32}$/i.test(hex)) return '未命名网络';
    try {
      const bytes = Uint8Array.from(hex.match(/../g), n => parseInt(n, 16));
      const value = new TextDecoder('utf-8', {fatal: true}).decode(bytes);
      if (/[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/u.test(value)) throw new Error();
      return value;
    } catch (_) { return '名称（HEX）' + hex.toUpperCase(); }
  }
  function clearPassword() { $('password').value = ''; $('password').type = 'password'; $('show-password').checked = false; }
  function notice(title, detail, tone = '') {
    $('state-title').textContent = title; $('state-detail').textContent = detail; $('notice').dataset.tone = tone;
  }
  function finish() {
    expired = true; session = null; pending = null; clearPassword(); clearTimeout(timer);
    $('chooser').hidden = true;
    for (const id of ['cancel', 'close', 'refresh']) $(id).hidden = true;
    $('time').textContent = '会话已结束';
    if (state?.phase === 'connected') notice('Wi-Fi 已保存', '手机配网已结束。请查看圆屏上的连接状态。', 'success');
    else notice('手机配网已结束', '请查看 MyBot 圆屏上的结果。需要再设置时，在圆屏重新打开手机配网。');
  }
  function editable() {
    return state && !offline && !pending && !state.command_pending && ['choosing', 'failed'].includes(state.phase);
  }
  function render() {
    if (expired) return;
    $('refresh').hidden = !offline;
    $('chooser').hidden = !state || !['choosing', 'failed'].includes(state.phase);
    $('close').hidden = !state;
    $('close').disabled = !!pending || offline;
    $('cancel').hidden = !state || (!pending && !['connecting', 'scanning', 'restoring'].includes(state.phase));
    $('cancel').disabled = offline || !state || (pending && !pending.uncertain) || !!state.command_pending;
    $('cancel').textContent = pending && pending.uncertain ? '取消未确认操作' : '取消当前操作';
    $('scan').disabled = !editable();
    if (!state) return;
    $('time').textContent = `剩余 ${Math.ceil(state.remaining_seconds / 60)} 分钟`;
    if (offline && state.phase === 'connected') notice('Wi-Fi 已保存', '临时热点已断开或手机配网已结束。最终连接状态请查看圆屏。', 'success');
    else if (offline) notice('暂时连不上 MyBot', '请保持连接 MyBot 临时热点，或在手机 Wi-Fi 设置中重新连接。页面会继续读取状态。', 'error');
    else if (pending && pending.uncertain) notice('正在确认设备状态', '刚才的操作尚未确认。请保持连接临时热点；也可以明确取消后重新选择。', 'busy');
    else if (pending || state.command_pending) notice('正在读取最新状态', '请稍候，设备正在处理设置。', 'busy');
    else if (state.phase === 'connected') notice('Wi-Fi 已保存', `${name(state.connected_ssid_hex)}。请查看圆屏连接状态；此结果不代表互联网已连通。`, 'success');
    else if (state.phase === 'connecting') notice('MyBot 正在连接', '热点可能短暂断开。请保持连接或重新连回 MyBot 临时热点，等待设备结果。', 'busy');
    else if (state.phase === 'saving') notice('正在保存 Wi-Fi', '请不要关闭设备。', 'busy');
    else if (state.phase === 'scanning') notice('正在搜索附近网络', '稍等片刻，网络列表即将更新。', 'busy');
    else if (state.phase === 'restoring') notice('正在恢复连接', '请等待设备完成，再重新选择网络。', 'busy');
    else if (state.phase === 'failed') notice('这次没有完成连接', errors[state.error] || '请查看圆屏提示后重试。', 'error');
    else if (state.phase === 'choosing') notice('已连接 MyBot', errors[state.error] || '请选择希望 MyBot 使用的 Wi-Fi。');
    else notice('设备准备中', '请稍候。', 'busy');
    renderNetworks();
  }
  function renderNetworks() {
    const fragment = document.createDocumentFragment();
    for (const network of state.networks) {
      const button = document.createElement('button');
      button.type = 'button'; button.className = 'network';
      button.setAttribute('aria-pressed', String(selected?.index === network.index));
      button.disabled = !editable();
      const dot = document.createElement('span'); dot.className = 'network-dot'; dot.setAttribute('aria-hidden', 'true');
      const copy = document.createElement('span'); copy.className = 'network-copy';
      const title = document.createElement('span'); title.className = 'network-name'; title.textContent = name(network.ssid_hex);
      const meta = document.createElement('span'); meta.className = 'network-meta';
      meta.textContent = {open: '开放网络 · 无需密码', personal: '密码保护', unsupported: '暂不支持此认证方式'}[network.security] || '暂不支持此认证方式';
      const strength = document.createElement('span'); strength.className = 'network-strength';
      strength.textContent = network.rssi > -60 ? '信号强' : network.rssi > -75 ? '信号中' : '信号弱';
      copy.append(title, meta); button.append(dot, copy, strength);
      button.addEventListener('click', () => { selected = network; clearPassword(); $('form-error').textContent = ''; renderNetworks(); if (network.security === 'personal') $('password').focus(); });
      fragment.append(button);
    }
    // Preserve focus while polling when the list has not changed.
    const key = JSON.stringify([state.scan_revision, state.networks, selected?.index, editable()]);
    if ($('networks').dataset.key !== key) { $('networks').replaceChildren(fragment); $('networks').dataset.key = key; }
    $('empty').hidden = state.networks.length > 0;
    $('selected').hidden = !selected;
    $('selected-name').textContent = selected ? name(selected.ssid_hex) : '';
    $('password-field').hidden = selected?.security !== 'personal';
    $('password').disabled = !editable();
    $('show-password').disabled = !editable();
    $('network-note').textContent = selected?.security === 'unsupported' ? 'MyBot 暂不支持企业认证等网络，请选择其他 Wi-Fi。' : selected?.security === 'open' ? '这个网络不需要密码。' : '';
    $('connect').disabled = !editable() || !selected || selected.security === 'unsupported';
    $('connect').textContent = !selected ? '选择网络后连接' : state.phase === 'failed' ? '重新连接' : '连接这个网络';
  }
  async function request(path, options = {}) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 4500);
    try {
      const response = await fetch(path, {...options, cache: 'no-store', credentials: 'omit', referrerPolicy: 'no-referrer', signal: controller.signal});
      const data = await response.json();
      return {status: response.status, ok: response.ok, data};
    }
    finally { clearTimeout(timeout); }
  }
  async function poll() {
    if (polling || expired) return;
    clearTimeout(timer); polling = true;
    try {
      if (!session) {
        const response = await request('/api/session');
        if (response.status === 410) { finish(); return; }
        if (!response.ok) throw new Error();
        session = response.data;
      }
      const response = await request('/api/status', {headers: {'X-Setup-Token': session.token}});
      if (response.status === 410 || response.status === 403) { finish(); return; }
      if (!response.ok) throw new Error();
      const next = response.data;
      if (next.operation !== session.operation || next.phone_phase !== 'serving' || next.phase === 'closed') { finish(); return; }
      if (state && next.revision < state.revision) return;
      if (state && state.scan_revision !== next.scan_revision) { selected = null; clearPassword(); }
      state = next; offline = false;
      if (pending && state.revision > pending.revision) pending = null;
      render();
    } catch (_) {
      offline = true;
      if (pending) pending.uncertain = true;
      if (!state) notice('暂时连不上 MyBot', '请连接圆屏显示的 MyBot 临时热点，页面会继续读取设备状态。', 'error');
      render();
    } finally { polling = false; if (!expired) timer = setTimeout(poll, offline ? 2200 : 1200); }
  }
  async function command(action) {
    if (!state || offline || expired || (pending && !(action === 'cancel' && pending.uncertain))) return;
    const payload = {operation: session.operation, revision: state.revision, action};
    if (action === 'connect') {
      if (!editable() || !selected) return;
      payload.scan_revision = state.scan_revision; payload.index = selected.index;
      payload.password = selected.security === 'personal' ? $('password').value : '';
      const bytes = new TextEncoder().encode(payload.password).length;
      if (selected.security === 'personal' && !((bytes >= 8 && bytes <= 63) || /^[\da-f]{64}$/i.test(payload.password))) {
        payload.password = ''; $('form-error').textContent = '请输入 8–63 字节密码，或 64 位十六进制密钥。'; return;
      }
    }
    const intent = {revision: state.revision, uncertain: false};
    pending = intent;
    let body = JSON.stringify(payload); payload.password = ''; clearPassword(); $('form-error').textContent = ''; render();
    try {
      const promise = request('/api/command', {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Setup-Token': session.token}, body});
      body = '';
      const response = await promise;
      if (response.status === 410 || response.status === 403) { finish(); return; }
      if (pending !== intent) return;
      if (response.status !== 202) {
        pending = null; state = null; selected = null; $('chooser').hidden = true;
        notice('正在重新读取设备状态', response.status === 409 ? '设备状态已变化。读取完成后，请重新选择操作。' : '操作未被接受，请读取状态后重试。', 'busy');
      }
    } catch (_) { if (pending === intent) { pending.uncertain = true; offline = true; render(); } }
    finally { body = ''; poll(); }
  }
  $('connect-form').addEventListener('submit', event => { event.preventDefault(); command('connect'); });
  $('scan').addEventListener('click', () => command('scan'));
  $('cancel').addEventListener('click', () => command('cancel'));
  $('close').addEventListener('click', () => command('close'));
  $('refresh').addEventListener('click', poll);
  $('show-password').addEventListener('change', () => { $('password').type = $('show-password').checked ? 'text' : 'password'; });
  window.addEventListener('pagehide', clearPassword);
  window.addEventListener('online', poll);
  poll();
})();
