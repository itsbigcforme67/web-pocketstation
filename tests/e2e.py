import sys, time
from playwright.sync_api import sync_playwright
OUT = 'shots/'
with sync_playwright() as p:
    b = p.chromium.launch()
    ctx = b.new_context(**p.devices['Pixel 7'])
    pg = ctx.new_page()
    logs = []
    pg.on('console', lambda m: logs.append(f'{m.type}: {m.text}'))
    pg.on('pageerror', lambda e: logs.append(f'PAGEERROR: {e}'))
    pg.goto('http://localhost:8765/')
    pg.wait_for_selector('#bios-banner:not(.hidden)', timeout=8000)
    pg.screenshot(path=OUT + '1-library-nobios.png', full_page=True)
    pg.set_input_files('#bios-input', 'tests/J110.bin')
    pg.wait_for_selector('#bios-banner.hidden', state='attached', timeout=5000)
    pg.wait_for_selector('.game', timeout=8000)
    print('games:', pg.locator('.game .name').all_inner_texts())
    pg.screenshot(path=OUT + '2-library.png', full_page=True)
    pg.locator('.game', has_text='TIC TAC').click()
    pg.wait_for_selector('#player.active')
    time.sleep(2.5)
    pg.screenshot(path=OUT + '3-tictactoe.png')
    fire = pg.locator('.pbtn.fire').bounding_box()
    right = pg.locator('.pbtn.right').bounding_box()
    def tap(box):
        x, y = box['x'] + box['width']/2, box['y'] + box['height']/2
        pg.mouse.move(x, y); pg.mouse.down(); time.sleep(0.15); pg.mouse.up(); time.sleep(0.6)
    tap(fire); tap(right); tap(fire)
    time.sleep(1)
    pg.screenshot(path=OUT + '4-tictactoe-moves.png')
    st = pg.evaluate('() => { const ps = window.__wps.player.ps; return {time: ps.time, pc: ps.cpu.pc.toString(16), clk: ps.clkMode} }')
    print('emu', st)
    pg.click('#btn-back')
    pg.wait_for_selector('#library.active')
    pg.locator('.game', has_text='チョコボ').click()
    time.sleep(3)
    pg.screenshot(path=OUT + '5-chocobo.png')
    print('\n'.join(logs[-20:]))
    b.close()
