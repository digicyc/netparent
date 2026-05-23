// netparent admin dashboard — vanilla JS.

const state = {
  selectedRouter: null,
  pollHandle: null,
};

const els = {
  routers:        document.getElementById('routers'),
  refreshBtn:     document.getElementById('refresh-btn'),
  section:        document.getElementById('devices-section'),
  currentId:      document.getElementById('current-router-id'),
  updated:        document.getElementById('devices-updated'),
  tbody:          document.querySelector('#devices-table tbody'),
};

async function api(path, opts = {}) {
  const res = await fetch(path, { credentials: 'same-origin', ...opts });
  if (res.status === 401) {
    window.location.href = '/login';
    throw new Error('unauthorized');
  }
  if (!res.ok) {
    const text = await res.text();
    throw new Error(`${res.status} ${text}`);
  }
  return res.json();
}

async function loadRouters() {
  const { routers } = await api('/api/routers');
  renderRouters(routers || []);
  if (!state.selectedRouter && routers && routers.length === 1) {
    selectRouter(routers[0].id);
  }
}

function renderRouters(routers) {
  if (routers.length === 0) {
    els.routers.innerHTML =
      '<p class="muted">No routers have connected yet. Once a netparent client connects to the broker it will appear here.</p>';
    return;
  }
  els.routers.innerHTML = routers.map(r => `
    <div class="router-card ${state.selectedRouter === r.id ? 'selected' : ''}" data-id="${escapeAttr(r.id)}">
      <div>
        <strong>${escapeHTML(r.id)}</strong>
        <div class="muted" style="font-size:0.8rem;">last update: ${formatTime(r.last_update)}</div>
      </div>
      <span class="status">
        <span class="dot ${r.online ? 'online' : 'offline'}"></span>
        ${r.online ? 'online' : 'offline'}
      </span>
    </div>
  `).join('');
  els.routers.querySelectorAll('.router-card').forEach(card => {
    card.addEventListener('click', () => selectRouter(card.dataset.id));
  });
}

async function selectRouter(id) {
  state.selectedRouter = id;
  await loadRouters(); // re-highlight
  await loadDevices();
  startPolling();
}

async function loadDevices() {
  if (!state.selectedRouter) return;
  try {
    const { router, devices } = await api(`/api/routers/${encodeURIComponent(state.selectedRouter)}/devices`);
    els.section.hidden = false;
    els.currentId.textContent = router.id;
    els.updated.textContent = router.devices_update
      ? `snapshot from ${formatTime(router.devices_update)}`
      : '';
    renderDevices(devices || []);
  } catch (e) {
    els.tbody.innerHTML = `<tr><td colspan="7" class="muted">Error loading devices: ${escapeHTML(e.message)}</td></tr>`;
  }
}

function renderDevices(devices) {
  if (devices.length === 0) {
    els.tbody.innerHTML = `<tr><td colspan="7" class="muted">No devices reported yet.</td></tr>`;
    return;
  }
  els.tbody.innerHTML = devices.map(d => `
    <tr>
      <td>${escapeHTML(d.hostname || '—')}</td>
      <td class="${d.vendor ? '' : 'muted'}">${escapeHTML(d.vendor || 'looking up…')}</td>
      <td><code>${escapeHTML(d.mac)}</code></td>
      <td>${escapeHTML(d.ip || '—')}</td>
      <td class="muted">${formatTime(d.last_seen)}</td>
      <td>
        <span class="badge ${d.blocked ? 'disabled' : 'enabled'}">
          ${d.blocked ? 'disabled' : 'enabled'}
        </span>
      </td>
      <td>
        ${d.blocked
          ? `<button class="btn-unblock" data-action="unblock" data-mac="${escapeAttr(d.mac)}">Enable</button>`
          : `<button class="btn-block"   data-action="block"   data-mac="${escapeAttr(d.mac)}">Disable</button>`}
      </td>
    </tr>
  `).join('');
  els.tbody.querySelectorAll('button[data-action]').forEach(btn => {
    btn.addEventListener('click', () => toggle(btn.dataset.action, btn.dataset.mac));
  });
}

async function toggle(action, mac) {
  try {
    await api(`/api/routers/${encodeURIComponent(state.selectedRouter)}/devices/${encodeURIComponent(mac)}/${action}`, {
      method: 'POST',
    });
    // Optimistic refresh — the router will publish a new devices snapshot
    // shortly, but we give it a moment then re-poll.
    setTimeout(loadDevices, 500);
  } catch (e) {
    alert(`Failed to ${action} ${mac}: ${e.message}`);
  }
}

function startPolling() {
  if (state.pollHandle) clearInterval(state.pollHandle);
  state.pollHandle = setInterval(() => {
    loadRouters();
    loadDevices();
  }, 5000);
}

function formatTime(epoch) {
  if (!epoch) return '—';
  const d = new Date(epoch * 1000);
  const now = new Date();
  const sameDay = d.toDateString() === now.toDateString();
  return sameDay
    ? d.toLocaleTimeString()
    : d.toLocaleString();
}

function escapeHTML(s) {
  return String(s ?? '').replace(/[&<>"']/g,
    c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c]));
}
function escapeAttr(s) { return escapeHTML(s); }

els.refreshBtn.addEventListener('click', () => {
  loadRouters();
  loadDevices();
});

loadRouters();
