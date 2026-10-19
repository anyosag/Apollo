<!-- [fork] The running version and over-the-air updates from the fork's master builds (src/fork/updater.h) -->
<template>
  <div class="card p-2 my-4">
    <div class="card-body" v-if="status">
      <h2>{{ $t('fork_update.version', { version: status.version }) }}</h2>
      <p class="text-body-secondary">
        {{ status.channel === 'master' ? $t('fork_update.build_master', { run: status.run }) : $t('fork_update.build_other') }}
      </p>
      <div class="alert alert-secondary" v-if="!status.supported">
        {{ $t('fork_update.unsupported') }}
      </div>
      <template v-else>
        <div class="alert" :class="alertClass">
          {{ status.message || $t('fork_update.not_checked') }}
        </div>
        <div class="mb-3" v-if="status.update && status.latest">
          <h3>{{ status.latest.version }}</h3>
          <pre class="mb-0">{{ status.latest.notes }}</pre>
        </div>
        <div class="alert alert-info" v-if="status.last_result">
          {{ $t('fork_update.last_install') }} {{ status.last_result.text }}
        </div>
        <div class="d-flex flex-wrap gap-2 align-items-center">
          <button class="btn btn-success" :disabled="busy || !status.update || status.state === 'installing'" @click="send({ action: 'install' })">
            {{ $t('fork_update.install') }}
          </button>
          <button class="btn btn-secondary" :disabled="busy || ['checking', 'downloading', 'installing'].includes(status.state)" @click="send({ action: 'check' })">
            {{ $t('fork_update.check') }}
          </button>
          <div class="form-check form-switch ms-2">
            <input class="form-check-input" type="checkbox" id="fork_auto_update" :checked="status.automatic" :disabled="busy" @change="send({ automatic: $event.target.checked })">
            <label class="form-check-label" for="fork_auto_update">{{ $t('fork_update.automatic') }}</label>
          </div>
        </div>
        <div class="form-text">{{ $t('fork_update.automatic_desc') }}</div>
        <div class="text-danger mt-2" v-if="error">{{ error }}</div>
      </template>
    </div>
    <div class="card-body" v-else>
      {{ $t('fork_update.loading') }}
    </div>
  </div>
</template>

<script setup>
  import { computed, onMounted, onUnmounted, ref } from 'vue'

  const status = ref(null)
  const busy = ref(false)
  const error = ref('')
  let timer = null

  const alertClass = computed(() => {
    switch (status.value?.state) {
      case 'failed':
        return 'alert-danger'
      case 'ready':
      case 'waiting':
      case 'installing':
        return 'alert-warning'
      default:
        return status.value?.update ? 'alert-warning' : 'alert-success'
    }
  })

  async function load() {
    try {
      const response = await fetch('./api/update')
      if (response.ok) {
        status.value = await response.json()
      }
    } catch (e) {
      console.error(e)
    }
  }

  async function send(body) {
    busy.value = true
    error.value = ''
    try {
      const response = await fetch('./api/update', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
      const json = await response.json()
      if (response.ok) {
        status.value = json
      } else {
        error.value = json.error || json.status_message || response.statusText
      }
    } catch (e) {
      error.value = String(e)
    }
    busy.value = false
  }

  onMounted(() => {
    load()
    timer = setInterval(load, 5000)
  })
  onUnmounted(() => clearInterval(timer))
</script>
