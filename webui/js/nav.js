'use strict';
/* Навігація - ЄДИНЕ джерело списку сторінок (Prompt 19). Підключати ПІСЛЯ common.js.
   Нова сторінка: поставити enabled: true (файл webui/<id>.html має існувати). */

const NAV_PAGES = [
  { id: 'home',     href: '/',                title: 'Головна',      enabled: true },
  { id: 'stations', href: '/stations.html',   title: 'Станції',      enabled: true },
  { id: 'audio',    href: '/audio.html',      title: 'Аудіо',        enabled: true },
  { id: 'ir',       href: '/ir.html',         title: 'Пульт',        enabled: true  },
  { id: 'settings', href: '/settings.html',   title: 'Налаштування', enabled: false },
  { id: 'system',   href: '/system.html',     title: 'Система',      enabled: false },
];

function pageById(id) { return NAV_PAGES.find((p) => p.id === id) || null; }

function renderNav(activeId) {
  const nav = $('#nav');
  if (!nav) return;
  nav.replaceChildren(...NAV_PAGES.map((p) => {
    if (!p.enabled) {
      return el('span', { 'aria-disabled': 'true', title: 'Скоро' }, p.title, el('small', null, 'скоро'));
    }
    return el('a', { href: p.href, 'aria-current': p.id === activeId ? 'page' : null }, p.title);
  }));
}
