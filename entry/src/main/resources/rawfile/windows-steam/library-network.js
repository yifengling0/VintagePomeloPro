/* Bound optional library content using the browser's own network timeout.
 * Restore caller settings on completion/reuse, including explicit later writes. */
(function () {
  'use strict';
  if (window.__vppLibraryOptionalNetworkV1 || typeof XMLHttpRequest !== 'function') return;
  var prototype = XMLHttpRequest.prototype;
  var originalOpen = prototype.open;
  var originalSend = prototype.send;
  var timeout = Object.getOwnPropertyDescriptor(prototype, 'timeout');
  if (!timeout || !timeout.get || !timeout.set || !timeout.configurable) return;
  var requests = new WeakMap();
  function limit(xhr, value) {
    timeout.set.call(xhr, value === 0 || value > 5000 ? 5000 : value);
  }
  Object.defineProperty(prototype, 'timeout', {
    get: timeout.get,
    set: function (value) {
      timeout.set.call(this, value);
      var state = requests.get(this);
      if (state && state.finish) {
        state.previousTimeout = timeout.get.call(this);
        limit(this, state.previousTimeout);
      }
    },
    enumerable: timeout.enumerable,
    configurable: timeout.configurable
  });
  function optionalContent(method, url, asynchronous) {
    if (String(method).toUpperCase() !== 'GET' || asynchronous === false) return false;
    try {
      var parsed = new URL(String(url), window.location.href);
      if (parsed.protocol !== 'https:' || parsed.port !== '' || parsed.username || parsed.password) return false;
      if (parsed.hostname === 'steamcommunity.com') {
        return /^\/profiles\/[0-9]+\/ajaxgetbadgeinfo\/[0-9]+\/?$/.test(parsed.pathname);
      }
      return parsed.hostname === 'store.steampowered.com' &&
        (parsed.pathname === '/broadcast/ajaxgetstreamersforpage' ||
         parsed.pathname === '/events/ajaxgetpartnerevent');
    } catch (_) { return false; }
  }
  prototype.open = function (method, url, asynchronous) {
    var previous = requests.get(this);
    // open(sync) must see the caller's original timeout, never our temporary
    // nonzero value (which would otherwise cause InvalidAccessError).
    if (previous && previous.finish) timeout.set.call(this, previous.previousTimeout);
    var result;
    try { result = originalOpen.apply(this, arguments); }
    catch (error) {
      if (previous && previous.finish) limit(this, previous.previousTimeout);
      throw error;
    }
    if (previous && previous.finish) previous.finish();
    var isAsync = arguments.length < 3 || asynchronous === undefined || Boolean(asynchronous);
    requests.set(this, { optional: optionalContent(method, url, isAsync), finish: null });
    return result;
  };
  prototype.send = function () {
    var state = requests.get(this);
    if (!state || !state.optional || (this.timeout > 0 && this.timeout <= 5000) || state.finish) {
      return originalSend.apply(this, arguments);
    }
    var xhr = this;
    state.previousTimeout = timeout.get.call(xhr);
    function finish() {
      if (state.finish !== finish) return;
      state.finish = null;
      xhr.removeEventListener('loadend', finish);
      timeout.set.call(xhr, state.previousTimeout);
    }
    state.finish = finish;
    xhr.addEventListener('loadend', finish);
    try {
      limit(xhr, state.previousTimeout);
      return originalSend.apply(xhr, arguments);
    } catch (error) { finish(); throw error; }
  };
  Object.defineProperty(window, '__vppLibraryOptionalNetworkV1', { value: true });
}());
