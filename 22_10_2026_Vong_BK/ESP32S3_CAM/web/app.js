const $ = (id) => document.getElementById(id);

let streamImage = $('stream');
const inspectionImage = $('inspection-image');
const learnImage = $('learn-image');
const overlay = $('overlay');
const overlayCtx = overlay.getContext('2d');
const inspectionStage = document.querySelector('.inspection-stage');

/* Chưa có ảnh -> ẩn <img> để không lộ icon ảnh vỡ + chữ alt như trong screenshot. */
streamImage.style.visibility = 'hidden';
inspectionImage.style.visibility = 'hidden';
learnImage.style.visibility = 'hidden';

let latestStatus = null;
let streamRetryTimer = null;
let lastStatusSeq = -1;
let lastLearnPowerKey = '';

/* Bản sao kết quả inspection cuối cùng (scores + hình học patch).
 * ESP32 xóa check.capture_id/scores về 0 ngay khi STM32 gửi CHECK_NEXT (nhất là sau
 * visual FAIL), trong khi ảnh lớn vẫn giữ ảnh cũ. Giữ bản sao này để khung ảnh lớn và
 * danh sách "Patch lỗi nổi bật" luôn khớp nhau, cho tới khi có ảnh inspection mới. */
let lastInspection = null;
let lastRenderKey = '';

/* Ảnh inspection: chỉ coi là "đã có ảnh" khi ảnh THỰC SỰ đã tải xong và hiển thị.
 * (Bản cũ dựa vào check.capture_id — luôn = 0 trong LEARN — nên lớp "Chưa có ảnh
 * inspection" bị bật lại sau mỗi lần poll dù ảnh đã nằm sẵn bên dưới.) */
let inspectionShownId = 0;      /* capture id của ảnh đang hiển thị */
let inspectionLoadingId = 0;    /* capture id đang tải */
let inspectionFailAt = 0;
let inspectionBlobUrl = null;
let learnImageShownId = 0;
let learnImageTargetId = 0;
let learnImageLoadingId = 0;
let learnImageFailAt = 0;
let learnImageBlobUrl = null;
let learnImageAbort = null;

const RESULT_PASS = 0;
const RESULT_FAIL = 1;

function setText(id, value) {
  const el = $(id);
  if (el) el.textContent = value;
}

function makePcbSticker() {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  svg.classList.add('pcb-sticker');
  svg.setAttribute('aria-hidden', 'true');
  use.setAttribute('href', '#pcb-sticker');
  svg.appendChild(use);
  return svg;
}

function clamp(v, min, max) {
  return Math.max(min, Math.min(max, v));
}

function formatFloat(value, digits = 2) {
  return Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : '—';
}

function formatMs(value) {
  return Number.isFinite(Number(value)) ? `${Math.round(Number(value))} ms` : '—';
}

function stageFamily(stage) {
  if (!stage) return 'STANDBY';
  if (stage.startsWith('LEARN')) return 'LEARN';
  if (stage.startsWith('TRAIN')) return 'TRAIN';
  if (stage.startsWith('CHECK')) return 'CHECK';
  return stage === 'ERROR' ? 'ERROR' : 'STANDBY';
}

function isCheckStage(stage) {
  return stage && stage.startsWith('CHECK_');
}

function updatePipeline(stage) {
  const family = stageFamily(stage);
  document.querySelectorAll('.pipeline-step').forEach((el) => {
    const step = el.dataset.step;
    el.classList.toggle('active', family === step && stage !== 'TRAINED');
    el.classList.toggle('done',
      (step === 'LEARN' && (stage === 'TRAINING' || stage === 'TRAINED' || family === 'CHECK')) ||
      (step === 'TRAIN' && (stage === 'TRAINED' || family === 'CHECK'))
    );
  });
  setText('learn-state', family === 'LEARN' ? 'ACTIVE' : (family === 'TRAIN' || family === 'CHECK' ? 'DONE' : 'WAIT'));
  setText('train-state', stage === 'TRAINED' || family === 'CHECK' ? 'DONE' : (family === 'TRAIN' ? 'ACTIVE' : 'WAIT'));
  setText('check-state', family === 'CHECK' ? 'ACTIVE' : 'WAIT');
}

function familyStage(stage) {
  return stageFamily(stage);
}

function powerResultText(result) {
  if (result === RESULT_PASS) return 'PASS';
  if (result === RESULT_FAIL) return 'FAIL';
  return '—';
}

function resultWord(r) {
  r = Number(r);
  return r === RESULT_PASS ? 'PASS' : (r === RESULT_FAIL ? 'FAIL' : '—');
}

function currentText(measured, mean, dev) {
  return `${formatFloat(measured, 1)} (chuẩn ${formatFloat(mean, 1)} ± ${formatFloat(dev, 1)})`;
}

let lastSummaryKey = '';

function updateSummary(status) {
  const s = status.summary || {};
  const hist = Array.isArray(status.history) ? status.history : [];
  const key = JSON.stringify([s, hist]);
  if (key === lastSummaryKey) return;          /* không vẽ lại khi dữ liệu không đổi */
  lastSummaryKey = key;

  const banner = $('summary-banner');
  const valid = !!s.valid;
  const total = Number(s.total) || 0;
  setText('summary-totals', `Đã kiểm ${total} PCB · PASS ${Number(s.pass) || 0} · FAIL ${Number(s.fail) || 0}`);

  if (!valid) {
    banner.className = 'result-banner neutral';
    setText('summary-board', '#—');
    setText('summary-text', 'CHƯA CÓ');
    setText('summary-sub', 'Chưa hoàn tất PCB nào trong phiên CHECK này.');
    ['sum-visual', 'sum-patches', 'sum-dmin', 'sum-vtime', 'sum-power', 'sum-imax', 'sum-iavg', 'sum-ptime']
      .forEach((id) => setText(id, '—'));
  } else {
    const pass = Number(s.final) === RESULT_PASS;
    const powerMeasured = Number(s.power) === RESULT_PASS || Number(s.power) === RESULT_FAIL;
    const patches = `${Number(s.patch_fail) || 0} / ${Number(s.patch_total) || 0}`;
    banner.className = `result-banner ${pass ? 'pass' : 'fail'}`;
    setText('summary-board', `#${s.board}`);
    setText('summary-text', `PCB #${s.board} · ${pass ? 'PASS' : 'FAIL'}`);
    setText('summary-sub', powerMeasured
      ? `Ngoại quan ${resultWord(s.visual)} (${patches} patch lỗi) · Dòng điện ${resultWord(s.power)}.`
      : `Ngoại quan FAIL: ${patches} patch lỗi · không đo dòng điện.`);
    setText('sum-visual', resultWord(s.visual));
    setText('sum-patches', patches);
    setText('sum-dmin', `${formatFloat(s.max_dmin, 3)} / ${formatFloat(s.v_threshold, 3)}`);
    setText('sum-vtime', formatMs(s.visual_ms));
    setText('sum-power', powerMeasured ? resultWord(s.power) : 'Không đo');
    setText('sum-imax', powerMeasured ? currentText(s.imax, s.imax_mean, s.imax_dev) : '—');
    setText('sum-iavg', powerMeasured ? currentText(s.iavg, s.iavg_mean, s.iavg_dev) : '—');
    setText('sum-ptime', powerMeasured ? formatMs(s.power_ms) : '—');
  }

  const body = $('history-body');
  if (!body) return;
  body.textContent = '';
  const rows = hist.slice().reverse();          /* mới nhất ở trên cùng */
  if (!rows.length) {
    const tr = document.createElement('tr');
    const td = document.createElement('td');
    const content = document.createElement('div');
    const title = document.createElement('strong');
    const note = document.createElement('span');
    td.colSpan = 6; td.className = 'history-empty';
    content.className = 'history-empty-content';
    title.textContent = 'Chưa có lượt kiểm tra';
    note.textContent = 'Dữ liệu PCB sẽ xuất hiện tại đây sau lần CHECK đầu tiên.';
    content.append(makePcbSticker(), title, note);
    td.appendChild(content);
    tr.appendChild(td); body.appendChild(tr);
    return;
  }
  rows.forEach((h) => {
    const measured = Number(h.power) === RESULT_PASS || Number(h.power) === RESULT_FAIL;
    const cells = [
      [`#${h.board}`, ''],
      [resultWord(h.visual), Number(h.visual) === RESULT_PASS ? 'pass' : 'fail'],
      [`${Number(h.patch_fail) || 0} / ${Number(h.patch_total) || 0}`, ''],
      [measured ? formatFloat(h.imax, 1) : '—', measured ? '' : 'skip'],
      [measured ? formatFloat(h.iavg, 1) : '—', measured ? '' : 'skip'],
      [resultWord(h.final), Number(h.final) === RESULT_PASS ? 'pass' : 'fail'],
    ];
    const tr = document.createElement('tr');
    cells.forEach(([text, cls]) => {
      const td = document.createElement('td');
      td.textContent = text;
      if (cls) td.className = cls;
      tr.appendChild(td);
    });
    body.appendChild(tr);
  });
}

function updateProcess(status) {
  const stage = status.stage || 'STANDBY';
  setText('stage-title', stage.replaceAll('_', ' '));
  setText('stage-code', stage);
  setText('process-message', status.last_event || 'Chờ lệnh từ STM32.');

  const learn = status.learn || {};
  const total = Number(learn.total) || 5;
  const visualDone = clamp(Number(learn.visual_done) || 0, 0, total);
  const powerDone = clamp(Number(learn.power_done) || 0, 0, total);
  setText('learn-visual-progress', `${visualDone} / ${total}`);
  setText('learn-power-progress', `${powerDone} / ${total}`);
  $('learn-visual-bar').style.width = `${(visualDone / total) * 100}%`;
  $('learn-power-bar').style.width = `${(powerDone / total) * 100}%`;
  setText('learn-power-badge', `${powerDone} / ${total}`);
  const samples = Array.isArray(learn.power_samples) ? learn.power_samples : [];
  const body = $('learn-power-body');
  const powerKey = JSON.stringify([total, powerDone, samples]);
  if (powerKey !== lastLearnPowerKey) {
    lastLearnPowerKey = powerKey;
    body.replaceChildren();
    for (let index = 1; index <= total; index += 1) {
      const sample = samples.find((item) => Number(item.index) === index);
      const row = document.createElement('tr');
      if (sample) row.className = index === powerDone ? 'latest' : 'recorded';
      [String(index).padStart(2, '0'), sample ? formatFloat(sample.imax, 2) : '—', sample ? formatFloat(sample.iavg, 2) : '—']
        .forEach((value) => {
          const cell = document.createElement('td');
          cell.textContent = value;
          row.appendChild(cell);
        });
      body.appendChild(row);
    }
  }

  const deletedSample = Number(learn.deleted_visual_sample) || 0;
  const recapture = $('recapture-notice');
  recapture.classList.toggle('hidden', !deletedSample);
  if (deletedSample) setText('recapture-title', `Đã xóa dữ liệu ảnh mẫu ${deletedSample}/${total}`);
  let nextStep = 'Chờ STM32 bắt đầu thu thập mẫu.';
  if (deletedSample) nextStep = `Chụp lại ảnh mẫu ${deletedSample}/${total} để tiếp tục.`;
  else if (stage === 'LEARN_CAPTURING') nextStep = 'Đang chụp và xử lý ảnh PCB mẫu.';
  else if (stage === 'LEARN_WAIT_VISUAL') nextStep = `Chờ ảnh ngoại quan mẫu ${Math.min(visualDone + 1, total)}/${total}.`;
  else if (stage === 'LEARN_RECEIVING_POWER' || stage === 'LEARN_WAIT_POWER') nextStep = `Chờ mẫu dòng ${Math.min(powerDone + 1, total)}/${total} từ STM32.`;
  else if (stage === 'LEARN_READY_TRAIN') nextStep = 'Đã đủ mẫu; chờ STM32 bắt đầu TRAIN.';
  else if (stage === 'TRAINING') nextStep = 'Đang tính Memory Bank và ngưỡng dòng.';
  else if (status.model_trained) nextStep = 'Đã học xong; xem giá trị TB bên dưới.';
  setText('learn-next-step', nextStep);

  setText('camera-status', status.camera ? 'ONLINE' : 'WAIT');
  setText('ai-status', status.ai_ready ? 'READY' : 'WAIT');
  setText('bank-status', status.bank_ready ? `${status.train?.bank_count || 0} vectors` : 'NOT TRAINED');
  setText('capture-id', status.last_capture_id || '—');
}

function updateTraining(status) {
  const train = status.train || {};
  const trained = !!status.model_trained;
  const pill = $('model-pill');
  pill.textContent = trained ? 'TRAINED' : 'NOT TRAINED';
  pill.className = `status-pill ${trained ? '' : 'neutral'}`;
  setText('train-bank', trained ? (train.bank_count ?? '—') : '—');
  setText('train-threshold', trained ? formatFloat(train.image_threshold, 3) : '—');
  setText('train-imax-mean', trained ? formatFloat(train.imax_mean, 2) : '—');
  setText('train-iavg-mean', trained ? formatFloat(train.iavg_mean, 2) : '—');
  setText('train-imax-dev', trained ? `± ${formatFloat(train.imax_max_dev, 2)}` : '—');
  setText('train-iavg-dev', trained ? `± ${formatFloat(train.iavg_max_dev, 2)}` : '—');
  setText('train-time', trained ? formatMs(train.elapsed_ms) : '—');
}

function updateSystem(status) {
  const cameraPill = $('camera-pill');
  cameraPill.textContent = status.camera ? 'ONLINE' : 'WAITING';
  cameraPill.className = `status-pill ${status.camera ? '' : 'neutral'}`;
  setText('camera-detail', status.camera ? 'UVC · MJPEG · camera gốc' : 'UVC · waiting');
  setText('live-message', status.camera ? 'Live preview' : 'Chờ camera UVC');
  setText('event-text', status.last_event || 'Đang chờ STM32...');
  setText('board-chip', `PCB ${status.board ? `#${status.board}` : '—'}`);
  setText('seq-chip', `SEQ ${status.event_seq ?? '—'}`);
  document.body.classList.toggle('has-error', status.stage === 'ERROR');
}

function updateInspectionPill(status) {
  const check = status.check || {};
  const stage = status.stage || '';
  const pill = $('visual-pill');
  let text = 'NO RESULT';
  let cls = 'status-pill neutral';
  if (stage === 'CHECK_VISUAL') { text = 'AI RUNNING'; cls = 'status-pill working'; }
  else if (stageFamily(stage) === 'LEARN') { text = `LEARN ${Number(status.learn?.visual_done) || 0}/${Number(status.learn?.total) || 5}`; cls = 'status-pill working'; }
  else if (check.visual_result === RESULT_FAIL || stage === 'CHECK_VISUAL_FAIL') { text = 'VISUAL FAIL'; cls = 'status-pill fail'; }
  else if (check.visual_result === RESULT_PASS) { text = 'VISUAL PASS'; cls = 'status-pill pass'; }
  pill.textContent = text;
  pill.className = cls;
}

function setInspectionMode(status) {
  const stage = status.stage || 'STANDBY';
  const hasImage = inspectionShownId > 0;
  const inspectionWorking = $('inspection-working');
  const placeholder = $('inspection-placeholder');

  const isCheckVisualWorking = stage === 'CHECK_VISUAL';
  const isLearnCapturing = stage === 'LEARN_CAPTURING';
  const isWorking = (isCheckVisualWorking || isLearnCapturing) && isStatusFresh();
  inspectionWorking.classList.toggle('hidden', !isWorking);
  inspectionWorking.classList.toggle('has-image', hasImage);
  /* Placeholder CHỈ hiện khi chưa từng có ảnh nào được hiển thị. */
  placeholder.classList.toggle('hidden', isWorking || hasImage);

  if (isCheckVisualWorking) {
    setText('working-title', `PCB #${status.board || '—'} · ESP32-S3 đang chụp + AI`);
    setText('working-subtitle', 'STM32 đang chờ VISUAL_RESULT');
  } else if (isLearnCapturing) {
    setText('working-title', 'LEARN · ESP32-S3 đang chụp mẫu');
    setText('working-subtitle', `Mẫu ngoại quan ${status.learn?.visual_working || '—'} / ${status.learn?.total || 5}`);
  }

  if (!hasImage) {
    let title = 'Chưa có ảnh inspection';
    let subtitle = 'Ảnh sẽ tự xuất hiện sau khi STM32 yêu cầu CAPTURE / VISUAL CHECK.';
    if (stage === 'CHECK_WAIT_PCB') {
      title = `Chờ PCB #${status.board || '—'}`;
      subtitle = 'Ảnh sẽ tự xuất hiện khi STM32 yêu cầu kiểm tra ngoại quan.';
    } else if (Number(status.learn?.deleted_visual_sample) > 0) {
      const sample = Number(status.learn.deleted_visual_sample);
      title = `Đã xóa dữ liệu ảnh mẫu ${sample}`;
      subtitle = `Vui lòng chụp lại mẫu ${sample} để tiếp tục LEARN.`;
    }
    placeholder.querySelector('strong').textContent = title;
    placeholder.querySelector('span').textContent = subtitle;
  }
}

function wantedCaptureId(status) {
  const check = status.check || {};
  if (stageFamily(status.stage) === 'LEARN' && Number(status.learn?.deleted_visual_sample) > 0) return 0;
  if (status.stage === 'LEARN_WAIT_VISUAL' && !Number(status.learn?.image_capture_id)) return 0;
  // The normalized image is published as soon as capture succeeds, before TFLite
  // invocations finish. Keep showing that latest frame in LEARN and CHECK;
  // scores still require their own matching check.capture_id to draw.
  return Math.max(Number(check.capture_id) || 0, Number(status.last_capture_id) || 0);
}

function refreshCaptureImage(status) {
  const id = wantedCaptureId(status);
  if (!id && inspectionShownId) {
    inspectionImage.removeAttribute('src');
    inspectionImage.style.visibility = 'hidden';
    if (inspectionBlobUrl) URL.revokeObjectURL(inspectionBlobUrl);
    inspectionBlobUrl = null;
    inspectionShownId = 0;
    setText('capture-detail', 'Chờ ảnh mẫu mới');
  }
  if (!id || id === inspectionShownId || inspectionLoadingId) return;
  /* Tải lỗi (ESP đang bận AI) -> chờ 1 s rồi tự thử lại ở lần poll kế tiếp. */
  if (Date.now() - inspectionFailAt < 1000) return;
  loadInspectionImage(id);
}

function refreshLearnImage(status) {
  const learn = status.learn || {};
  const id = Number(learn.image_capture_id) || 0;
  const sample = Number(learn.image_sample) || 0;

  if (id !== learnImageTargetId) {
    learnImageTargetId = id;
    learnImageAbort?.abort();
    learnImageAbort = null;
    learnImageLoadingId = 0;
    learnImageFailAt = 0;
    learnImage.removeAttribute('src');
    learnImage.style.visibility = 'hidden';
    $('learn-image-placeholder').classList.remove('hidden');
    if (learnImageBlobUrl) URL.revokeObjectURL(learnImageBlobUrl);
    learnImageBlobUrl = null;
    learnImageShownId = 0;
    setText('learn-image-placeholder-text', id ? 'Đang tải ảnh mẫu LEARN...' :
      (Number(learn.deleted_visual_sample) > 0 ? `Đã xóa ảnh mẫu ${learn.deleted_visual_sample}. Vui lòng chụp lại.` : 'Chưa có ảnh mẫu LEARN'));
  }

  setText('learn-image-label', id ? `Mẫu ${sample}/${learn.total || 5} · Capture #${id}` : 'Chờ ảnh mẫu');
  if (!id) setText('learn-image-placeholder-text', Number(learn.deleted_visual_sample) > 0
    ? `Đã xóa ảnh mẫu ${learn.deleted_visual_sample}. Vui lòng chụp lại.` : 'Chưa có ảnh mẫu LEARN');
  if (!id || id === learnImageShownId || learnImageLoadingId || Date.now() - learnImageFailAt < 1000) return;
  if (id < Number(status.last_capture_id) && stageFamily(status.stage) !== 'LEARN') {
    if (!learnImageShownId) setText('learn-image-placeholder-text', 'Ảnh mẫu đã được thay bằng ảnh CHECK; chờ lượt LEARN mới.');
    return;
  }
  loadLearnImage(id);
}

async function loadLearnImage(id) {
  const ctrl = new AbortController();
  learnImageAbort = ctrl;
  learnImageLoadingId = id;
  const timer = setTimeout(() => ctrl.abort(), 8000);
  let url = null;
  try {
    const response = await fetch(`/api/normalized-image?id=${id}&t=${Date.now()}`, { cache: 'no-store', signal: ctrl.signal });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = await response.blob();
    if (blob.size < 100) throw new Error('empty image');
    url = URL.createObjectURL(blob);
    const decoded = new Image();
    decoded.src = url;
    await decoded.decode();
    if (learnImageTargetId !== id) return;
    learnImage.src = url;
    await learnImage.decode();
    if (learnImageTargetId !== id) return;
    if (learnImageBlobUrl) URL.revokeObjectURL(learnImageBlobUrl);
    learnImageBlobUrl = url;
    url = null;
    learnImage.style.visibility = 'visible';
    learnImageShownId = id;
    $('learn-image-placeholder').classList.add('hidden');
  } catch (error) {
    if (learnImageTargetId === id) {
      learnImageFailAt = Date.now();
      console.warn('Tải ảnh mẫu LEARN lỗi:', error);
      if (!learnImageShownId) setText('learn-image-placeholder-text', 'Đang thử tải lại ảnh mẫu...');
    }
  } finally {
    if (url) URL.revokeObjectURL(url);
    clearTimeout(timer);
    if (learnImageLoadingId === id) learnImageLoadingId = 0;
    if (learnImageAbort === ctrl) learnImageAbort = null;
  }
}

/* Tải ảnh bằng fetch -> blob: ảnh cũ vẫn hiển thị cho tới khi ảnh mới sẵn sàng
 * (không chớp trắng/đen), và có timeout + retry thay vì bỏ cuộc im lặng. */
async function loadInspectionImage(id) {
  inspectionLoadingId = id;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 8000);
  let url = null;
  try {
    const response = await fetch(`/api/normalized-image?id=${id}&t=${Date.now()}`, { cache: 'no-store', signal: ctrl.signal });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = await response.blob();
    if (blob.size < 100) throw new Error('empty image');
    url = URL.createObjectURL(blob);
    const decoded = new Image();
    decoded.src = url;
    await decoded.decode();
    if (!latestStatus || wantedCaptureId(latestStatus) !== id) return;
    await new Promise((resolve, reject) => {
      inspectionImage.onload = resolve;
      inspectionImage.onerror = () => reject(new Error('display decode failed'));
      inspectionImage.src = url;
    });
    if (inspectionBlobUrl) URL.revokeObjectURL(inspectionBlobUrl);
    inspectionBlobUrl = url;
    url = null;
    inspectionImage.style.visibility = 'visible';
    inspectionShownId = id;
    inspectionFailAt = 0;
    $('inspection-placeholder').classList.add('hidden');
    if (latestStatus) {
      setInspectionMode(latestStatus);
      drawOverlay();
      renderFailedPatches(latestStatus);
    }
    const check = latestStatus?.check || {};
    setText('capture-detail', `Capture #${id} · PCB đã chuẩn hóa ${inspectionImage.naturalWidth}×${inspectionImage.naturalHeight}`);
  } catch (error) {
    inspectionFailAt = Date.now();
    console.warn('Tải ảnh inspection lỗi:', error);
    if (inspectionShownId === 0) setText('capture-detail', `Đang thử lại ảnh capture #${id}...`);
  } finally {
    if (url) URL.revokeObjectURL(url);
    clearTimeout(timer);
    if (inspectionLoadingId === id) inspectionLoadingId = 0;
  }
}

function drawOverlay() {
  const status = latestStatus;
  if (!status) return;
  const check = lastInspection || {};
  if (status.stage === 'CHECK_VISUAL' || !inspectionImage.naturalWidth || !inspectionImage.naturalHeight || !inspectionStage.clientWidth || !inspectionStage.clientHeight) {
    overlay.width = 1;
    overlay.height = 1;
    return;
  }

  const cols = Number(check.cols) || 0;
  const rows = Number(check.rows) || 0;
  const cropWidth = Number(check.crop_width) || 0;
  const cropHeight = Number(check.crop_height) || 0;
  const displayedCrop = inspectionImage.naturalWidth === cropWidth && inspectionImage.naturalHeight === cropHeight;
  const cropX = displayedCrop ? 0 : (Number(check.crop_x) || 0);
  const cropY = displayedCrop ? 0 : (Number(check.crop_y) || 0);
  const patchSize = Number(check.patch_size) || 64;
  const stride = Number(check.patch_stride) || 64;
  const threshold = Number(check.threshold);
  const scores = Array.isArray(check.scores) ? check.scores : [];
  if (!cols || !rows || !cropWidth || !cropHeight || !Number.isFinite(threshold) || !scores.length || Number(check.capture_id) <= 0 || Number(check.capture_id) !== inspectionShownId) {
    overlay.width = 1;
    overlay.height = 1;
    overlayCtx.clearRect(0, 0, 1, 1);
    return;
  }

  const rect = inspectionStage.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  overlay.width = Math.max(1, Math.round(rect.width * dpr));
  overlay.height = Math.max(1, Math.round(rect.height * dpr));
  overlay.style.width = `${rect.width}px`;
  overlay.style.height = `${rect.height}px`;
  overlayCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
  overlayCtx.clearRect(0, 0, rect.width, rect.height);

  const imageRatio = inspectionImage.naturalWidth / inspectionImage.naturalHeight;
  const boxRatio = rect.width / rect.height;
  let drawWidth, drawHeight, imageLeft, imageTop;
  if (imageRatio > boxRatio) {
    drawWidth = rect.width;
    drawHeight = drawWidth / imageRatio;
    imageLeft = 0;
    imageTop = (rect.height - drawHeight) / 2;
  } else {
    drawHeight = rect.height;
    drawWidth = drawHeight * imageRatio;
    imageTop = 0;
    imageLeft = (rect.width - drawWidth) / 2;
  }

  const scaleX = drawWidth / inspectionImage.naturalWidth;
  const scaleY = drawHeight / inspectionImage.naturalHeight;

  // Dim areas outside the AI image for older firmware responses.
  if (!displayedCrop) {
    overlayCtx.fillStyle = 'rgba(10, 18, 28, 0.48)';
    overlayCtx.fillRect(imageLeft, imageTop, drawWidth, cropY * scaleY);
    overlayCtx.fillRect(imageLeft, imageTop + (cropY + cropHeight) * scaleY,
      drawWidth, Math.max(0, inspectionImage.naturalHeight - cropY - cropHeight) * scaleY);
    overlayCtx.fillRect(imageLeft, imageTop + cropY * scaleY, cropX * scaleX, cropHeight * scaleY);
    overlayCtx.fillRect(imageLeft + (cropX + cropWidth) * scaleX, imageTop + cropY * scaleY,
      Math.max(0, inspectionImage.naturalWidth - cropX - cropWidth) * scaleX, cropHeight * scaleY);
  }

  // Draw a very light grid, then strongly highlight only failing patches.
  overlayCtx.lineWidth = 1;
  overlayCtx.strokeStyle = 'rgba(255,255,255,0.16)';
  for (let r = 0; r < rows; r += 1) {
    for (let c = 0; c < cols; c += 1) {
      const i = r * cols + c;
      const x = imageLeft + (cropX + c * stride) * scaleX;
      const y = imageTop + (cropY + r * stride) * scaleY;
      const cellW = patchSize * scaleX;
      const cellH = patchSize * scaleY;
      overlayCtx.strokeRect(x, y, cellW, cellH);
      const score = Number(scores[i]);
      if (Number.isFinite(score)) {
        const maxScore = Math.max(Number(check.max_dmin) || 0, threshold, 1e-6);
        const intensity = Math.max(0.035, Math.min(0.20, 0.20 * score / maxScore));
        overlayCtx.fillStyle = score > threshold
          ? 'rgba(216, 33, 56, 0.28)'
          : `rgba(255, 166, 0, ${intensity})`;
        overlayCtx.fillRect(x, y, cellW, cellH);
      }
      if (Number.isFinite(score) && score > threshold) {
        overlayCtx.strokeStyle = '#d82138';
        overlayCtx.lineWidth = Math.max(2, dpr);
        overlayCtx.strokeRect(x + 1, y + 1, Math.max(0, cellW - 2), Math.max(0, cellH - 2));
        overlayCtx.strokeStyle = 'rgba(255,255,255,0.16)';
        overlayCtx.lineWidth = 1;
      }
    }
  }
}

function renderFailedPatches(status) {
  const list = $('patch-list');
  list.innerHTML = '';
  const stage = (status && status.stage) || '';
  const showPlaceholder = (message) => {
    const node = document.createElement('div');
    const title = document.createElement('strong');
    const note = document.createElement('span');
    node.className = 'patch-empty placeholder';
    title.textContent = 'Chưa có vùng lỗi';
    note.textContent = message;
    node.append(makePcbSticker(), title, note);
    list.appendChild(node);
  };
  if (stage === 'CHECK_VISUAL') {
    showPlaceholder('Đang chờ điểm phân tích ảnh hiện tại.');
    setText('danger-count', '0');
    return;
  }
  const insp = lastInspection;
  if (!insp || !inspectionImage.naturalWidth || insp.capture_id !== inspectionShownId) {
    showPlaceholder('Đang chờ điểm phân tích ảnh hiện tại.');
    setText('danger-count', '0');
    return;
  }
  const { cols, rows, crop_width: cropWidth, crop_height: cropHeight,
    crop_x: cropX0 = 0, crop_y: cropY0 = 0, patch_size: patchSize = 64,
    patch_stride: stride = 64, threshold, scores, d1 = [], d3 = [] } = insp;
  if (!cols || !rows || !cropWidth || !cropHeight || !Number.isFinite(threshold) || !scores?.length) {
    showPlaceholder('Đang chờ điểm phân tích ảnh hiện tại.');
    setText('danger-count', '0');
    return;
  }
  const ranked = scores.map((score, index) => ({ score: Number(score), index }))
    .filter((item) => Number.isFinite(item.score))
    .sort((a, b) => b.score - a.score);
  const failed = ranked.filter((item) => item.score > threshold);
  setText('danger-count', String(failed.length));
  const shown = failed.length ? failed : ranked.slice(0, 12);
  const displayedCrop = inspectionImage.naturalWidth === cropWidth && inspectionImage.naturalHeight === cropHeight;
  const cropX = displayedCrop ? 0 : cropX0;
  const cropY = displayedCrop ? 0 : cropY0;
  if (!failed.length) {
    const note = document.createElement('div');
    note.className = 'patch-empty';
    note.textContent = 'Không có patch vượt ngưỡng · 12 điểm cao nhất:';
    list.appendChild(note);
  }
  shown.forEach(({ score, index }) => {
    const row = Math.floor(index / cols);
    const col = index % cols;
    const patchX = cropX + col * stride;
    const patchY = cropY + row * stride;
    const wrap = document.createElement('div');
    wrap.className = 'patch-card';
    const canvas = document.createElement('canvas');
    canvas.width = 120;
    canvas.height = 120;
    const ctx = canvas.getContext('2d');
    ctx.drawImage(inspectionImage, patchX, patchY, patchSize, patchSize, 0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = score > threshold ? '#d82138' : '#e4a11b';
    ctx.lineWidth = 5;
    ctx.strokeRect(2.5, 2.5, canvas.width - 5, canvas.height - 5);
    wrap.appendChild(canvas);
    const meta = document.createElement('div');
    meta.className = 'patch-meta';
    meta.textContent = `Patch ${index + 1} | row ${row + 1}, col ${col + 1} | score ${score.toFixed(4)} / ${threshold.toFixed(4)} | d1 ${Number(d1[index]).toFixed(4)} | d3 ${Number(d3[index]).toFixed(4)}`;
    wrap.appendChild(meta);
    list.appendChild(wrap);
  });
  const details = document.createElement('details');
  const summary = document.createElement('summary');
  summary.textContent = `Toàn bộ ${ranked.length} điểm patch`;
  details.appendChild(summary);
  const values = document.createElement('p');
  values.className = 'patch-score-all';
  values.textContent = scores.map((score, i) => `${i}: ${Number(score).toFixed(4)}`).join(' | ');
  details.appendChild(values);
  list.appendChild(details);
}

function rememberInspection(status) {
  if (stageFamily(status.stage) === 'LEARN') {
    lastInspection = null;           /* phiên LEARN mới: bỏ kết quả CHECK cũ */
    return;
  }
  const check = status.check || {};
  const scores = Array.isArray(check.scores) ? check.scores : [];
  const id = Number(check.capture_id) || 0;
  if (id > 0 && scores.length && Number(check.cols) > 0 && Number(check.rows) > 0) {
    if (!lastInspection || lastInspection.capture_id !== id) {
      lastInspection = {
        capture_id: id,
        cols: Number(check.cols),
        rows: Number(check.rows),
        crop_width: Number(check.crop_width) || 0,
        crop_height: Number(check.crop_height) || 0,
        crop_x: Number(check.crop_x) || 0,
        crop_y: Number(check.crop_y) || 0,
        patch_size: Number(check.patch_size) || 64,
        patch_stride: Number(check.patch_stride) || 64,
        threshold: Number(check.threshold),
        scores: scores.slice(),
        d1: Array.isArray(check.d1) ? check.d1.slice() : [],
        d3: Array.isArray(check.d3) ? check.d3.slice() : [],
      };
    }
  }
}

function applyStatus(status) {
  latestStatus = status;
  rememberInspection(status);
  updateSystem(status);
  updatePipeline(status.stage);
  updateProcess(status);
  refreshLearnImage(status);
  updateSummary(status);
  updateTraining(status);
  updateInspectionPill(status);
  refreshCaptureImage(status);
  setInspectionMode(status);

  const check = status.check || {};
  const failCount = Number(check.patch_fail_count) || 0;
  if (check.capture_id > 0) {
    setText('overlay-detail', failCount > 0 ? `${failCount} patch đỏ · vượt ngưỡng` : 'Visual PASS · không có patch đỏ');
    setText('capture-detail', `Capture #${check.capture_id} · PCB 640×448 · ${check.cols || 0}×${check.rows || 0} patch · ${formatMs(check.elapsed_ms)}`);
  }

  /* Vẽ lại overlay + danh sách patch khi bộ dữ liệu hoặc ảnh đang hiển thị đổi,
   * hoặc khi chuyển giữa các giai đoạn chờ/đo. Dùng cùng một nguồn dữ liệu
   * (lastInspection) nên ảnh lớn và danh sách patch luôn khớp nhau. */
  const waiting = status.stage === 'CHECK_VISUAL';
  const key = `${lastInspection ? lastInspection.capture_id : 0}|${inspectionShownId}|${waiting ? 'w' : 'o'}`;
  if (key !== lastRenderKey) {
    lastRenderKey = key;
    drawOverlay();
    renderFailedPatches(status);
  }
}

let statusFailCount = 0;
let lastUptime = -1;
let lastStatusOkAt = 0;
const STATUS_TIMEOUT_MS = 4000;
const STATUS_FAIL_OFFLINE = 5;      /* chỉ báo OFFLINE sau 5 lần lỗi liên tiếp */

function isStatusFresh() {
  return Date.now() - lastStatusOkAt < 3000;
}

async function refreshStatus() {
  /* Timeout + chạy tuần tự (không chồng request): trước đây setInterval 500 ms
   * không có timeout nên khi ESP32 bận (chụp/AI) các request chồng lên nhau,
   * httpd nghẽn và trạng thái web bị đứng ở "đang chụp". */
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), STATUS_TIMEOUT_MS);
  try {
    const response = await fetch('/api/status', { cache: 'no-store', signal: ctrl.signal });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const status = await response.json();
    statusFailCount = 0;
    lastStatusOkAt = Date.now();
    if (Number.isFinite(Number(status.uptime_s))) {
      if (lastUptime >= 0 && Number(status.uptime_s) + 2 < lastUptime) {
        /* uptime giảm => ESP32 vừa reset (brown-out / crash / WDT) */
        console.warn('ESP32 vừa khởi động lại, reset_reason =', status.reset_reason);
        setText('event-text', `ESP32 vừa khởi động lại (lý do: ${status.reset_reason || '?'})`);
        startStream(true);
      }
      lastUptime = Number(status.uptime_s);
    }
    applyStatus(status);
  } catch (error) {
    statusFailCount++;
    /* Nếu video vẫn đang về thì ESP32 còn sống, chỉ là /api/status chậm. */
    const streamAlive = Date.now() - lastFrameAt < 3000;
    if (statusFailCount >= STATUS_FAIL_OFFLINE && !streamAlive) {
      setText('event-text', 'Mất kết nối ESP32-S3 Web Console - đang thử lại...');
      $('camera-pill').textContent = 'OFFLINE';
      $('camera-pill').className = 'status-pill fail';
      /* Mất kết nối thì không để spinner "đang chụp" quay mãi */
      $('inspection-working').classList.add('hidden');
    }
  } finally {
    clearTimeout(timer);
    setTimeout(refreshStatus, statusFailCount ? 800 : 400);
  }
}

/* ---------------- LIVE STREAM (MJPEG qua fetch) ----------------
 * Bản cũ gán thẳng <img src=".../stream">. Với multipart/x-mixed-replace trình
 * duyệt KHÔNG báo lỗi khi luồng treo/đứt nên khung cứ đen hoặc đứng hình mà JS
 * không biết để nối lại. Bản này tự đọc luồng, tách từng JPEG, và:
 *  - hiển thị frame mới bằng blob (ảnh cũ giữ nguyên tới khi frame mới giải mã xong
 *    => không chớp đen),
 *  - watchdog: >4 s không có frame => huỷ kết nối và nối lại (backoff),
 *  - chỉ có đúng MỘT kết nối stream tại mọi thời điểm,
 *  - tab bị ẩn thì ngắt stream để nhường băng thông Wi-Fi cho ESP32. */
const STREAM_URL = `http://${location.hostname}:81/stream`;
const STREAM_STALL_MS = 4000;
const STREAM_MAX_FRAME = 2 * 1024 * 1024;
let streamAbort = null;
let streamRunning = false;
let streamWanted = true;
let streamBackoffMs = 500;
let streamBlobUrl = null;
let lastFrameAt = 0;
let fetchFailStreak = 0;
let imgFallback = false;
const latin1 = new TextDecoder('latin1');
const HEADER_RE = /--vedcframe\r\nContent-Type: image\/jpeg\r\nContent-Length: (\d+)\r\n\r\n/;

function showStreamFrame(bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'image/jpeg' }));
  const prev = streamBlobUrl;
  streamBlobUrl = url;
  streamImage.src = url;
  streamImage.style.visibility = 'visible';
  if (prev) URL.revokeObjectURL(prev);
}

async function runStream() {
  if (streamRunning) return;
  streamRunning = true;
  const ctrl = new AbortController();
  streamAbort = ctrl;
  lastFrameAt = Date.now();
  const watchdog = setInterval(() => {
    if (Date.now() - lastFrameAt > STREAM_STALL_MS) ctrl.abort();
  }, 1000);

  let buf = new Uint8Array(256 * 1024);
  let len = 0;
  let gotFrame = false;
  try {
    const response = await fetch(`${STREAM_URL}?t=${Date.now()}`, { mode: 'cors', signal: ctrl.signal });
    if (!response.ok || !response.body) throw new Error(`HTTP ${response.status}`);
    const reader = response.body.getReader();
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (len + value.length > buf.length) {
        const bigger = new Uint8Array(Math.max(buf.length * 2, len + value.length));
        bigger.set(buf.subarray(0, len));
        buf = bigger;
      }
      buf.set(value, len);
      len += value.length;

      /* tách tất cả frame hoàn chỉnh đang có trong buffer; chỉ hiển thị frame cuối */
      let latest = null;
      while (true) {
        const m = HEADER_RE.exec(latin1.decode(buf.subarray(0, Math.min(len, 512))));
        if (!m) {
          if (len > 2048) { buf.copyWithin(0, len - 256, len); len = 256; }   /* mất đồng bộ -> bỏ rác */
          break;
        }
        const size = Number(m[1]);
        const start = m.index + m[0].length;
        if (!size || size > STREAM_MAX_FRAME) { buf.copyWithin(0, start, len); len -= start; continue; }
        if (len < start + size) break;                                          /* chưa đủ dữ liệu */
        latest = buf.slice(start, start + size);
        buf.copyWithin(0, start + size, len);
        len -= start + size;
      }
      if (latest) {
        showStreamFrame(latest);
        lastFrameAt = Date.now();
        if (!gotFrame) { gotFrame = true; streamBackoffMs = 500; setText('live-message', 'Live preview'); }
      }
    }
  } catch (error) {
    /* AbortError do watchdog/ẩn tab là bình thường */
  } finally {
    clearInterval(watchdog);
    streamRunning = false;
    streamAbort = null;
    fetchFailStreak = gotFrame ? 0 : fetchFailStreak + 1;
    if (streamWanted && fetchFailStreak >= 3 && !imgFallback) {
      startImgFallback();               /* fetch bị chặn/không hỗ trợ -> dùng <img> MJPEG kiểu cũ */
    } else if (streamWanted && !imgFallback) {
      setText('live-message', 'Đang kết nối lại camera...');   /* frame cuối vẫn hiển thị */
      clearTimeout(streamRetryTimer);
      streamRetryTimer = setTimeout(runStream, streamBackoffMs);
      streamBackoffMs = Math.min(streamBackoffMs * 2, 4000);
    }
  }
}

/* Đường lùi: <img src=/stream> thuần. Trình duyệt tự giải multipart; nếu lỗi thì
 * gán lại src sau 1.5 s (không đụng tới DOM nên frame cuối vẫn hiển thị). */
function startImgFallback() {
  imgFallback = true;
  console.warn('Stream fetch thất bại nhiều lần, chuyển sang <img> MJPEG');
  const connect = () => {
    if (!streamWanted) return;
    streamImage.src = `${STREAM_URL}?t=${Date.now()}`;
  };
  streamImage.onload = () => {
    streamImage.style.visibility = 'visible';
    lastFrameAt = Date.now();
    setText('live-message', 'Live preview');
  };
  streamImage.onerror = () => {
    setText('live-message', 'Đang kết nối lại camera...');
    clearTimeout(streamRetryTimer);
    streamRetryTimer = setTimeout(connect, 1500);
  };
  connect();
}

function startStream(force = false) {
  streamWanted = true;
  if (imgFallback) { streamImage.src = `${STREAM_URL}?t=${Date.now()}`; return; }
  if (streamRunning) {
    if (force && streamAbort) streamAbort.abort();   /* finally{} sẽ nối lại */
    return;
  }
  clearTimeout(streamRetryTimer);
  runStream();
}

function stopStream() {
  streamWanted = false;
  clearTimeout(streamRetryTimer);
  if (streamAbort) streamAbort.abort();
  if (imgFallback) streamImage.removeAttribute('src');   /* ngắt kết nối khi ẩn tab */
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopStream();
  else { streamBackoffMs = 500; startStream(); }
});
window.addEventListener('pagehide', stopStream);
window.addEventListener('resize', () => {
  drawOverlay();
  if (latestStatus) renderFailedPatches(latestStatus);
});

startStream();
refreshStatus();
