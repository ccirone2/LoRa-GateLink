// The Install tab works without a board, per role, and fits a phone.
import { test, expect, openConsole, connect } from './helpers.js';

test('wiring per board, from the WIRING table', async ({ page }) => {
  await openConsole(page, [], { hash: 'install' });
  await page.locator('[data-wiring="gate"]').click();
  await expect(page.locator('#wiringTable')).toContainText('OUT2 · closed limit');
  await expect(page.locator('#wiringSvg')).toHaveAttribute('aria-label', /Gate board field wiring/);
  await page.locator('[data-wiring="house"]').click();
  await expect(page.locator('#wiringTable')).toContainText('Controller switch input');
  await expect(page.locator('#wiringNotes li')).not.toHaveCount(0);
  await expect(page.locator('#wiringCheck')).toBeHidden(); // the connected board's: none here
});

for (const width of [320, 375]) {
  test(`a ${width} px phone gets the compact diagram and every tab, without horizontal scroll`, async ({ page }) => {
    await page.setViewportSize({ width, height: 800 });
    await openConsole(page, [], { hash: 'install' });
    await expect(page.locator('#wiringSvg')).toHaveAttribute('viewBox', /^0 0 360 /);
    const overflow = await page.evaluate(() => [document.documentElement, document.getElementById('tabs')]
      .map((el) => el.scrollWidth - el.clientWidth));
    expect(overflow).toEqual([0, 0]);
  });
}

for (const [old, now] of [['tools', 'link'], ['security', 'board']]) {
  test(`a #${old} link from before the tabs were split opens ${now}`, async ({ page }) => {
    await openConsole(page, [], { hash: old });
    await expect(page.locator(`#tab-${now}`)).toBeVisible();
    await expect(page.locator(`#tabbtn-${now}`)).toHaveAttribute('aria-selected', 'true');
  });
}

test('the setup checklist ticks what the connected board reports', async ({ page }) => {
  await openConsole(page, [{ role: 'house', keySet: false }], { hash: 'install' });
  await expect(page.locator('#setupCard')).toBeHidden(); // no board yet
  await connect(page);
  const items = page.locator('#setupList li');
  await expect(items).toHaveCount(5);
  await expect(items.nth(0)).toHaveClass('done'); // role
  await expect(items.nth(1)).not.toHaveClass('done'); // no key
  await expect(items.nth(1)).toContainText('· Board');
  await expect(items.nth(2)).toContainText('The gate board connected here once');
  await expect(items.nth(4)).not.toHaveClass('done'); // no survey this session
});
