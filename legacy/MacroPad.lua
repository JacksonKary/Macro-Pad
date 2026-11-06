lmc_sleep(1000)  -- wait a second for device init
lmc_device_set_name('MACRO', '\\\\?\\HID#VID_514C&PID_8851&MI_01&COL01#8&3CBA26A&0&0000#{884B96C3-56EF-11D1-BC8C-00A0C91405DD}')

-- Virtual key codes for media and volume keys
local VK_MEDIA_NEXT_TRACK = 0xB0
local VK_MEDIA_PREV_TRACK = 0xB1
local VK_MEDIA_STOP       = 0xB2
local VK_MEDIA_PLAY_PAUSE = 0xB3
local VK_VOLUME_MUTE      = 0xAD
local VK_VOLUME_DOWN      = 0xAE
local VK_VOLUME_UP        = 0xAF

-- Helper function to "press" a key (down + up)
local function press_vk(vk)
  lmc_send_input(vk, 0, 0)  -- key down
  lmc_send_input(vk, 0, 2)  -- key up
end

-- Main handler
lmc_set_handler('MACRO', function(button, direction)
  if direction == 1 then
    if (button == string.byte('A')) then
      press_vk(VK_MEDIA_PREV_TRACK)
    elseif (button == string.byte('B')) then
      press_vk(VK_MEDIA_PLAY_PAUSE)
    elseif (button == string.byte('C')) then
      press_vk(VK_MEDIA_NEXT_TRACK)
    elseif (button == string.byte('1')) then
      press_vk(VK_VOLUME_DOWN)
    elseif (button == string.byte('2')) then
      press_vk(VK_VOLUME_MUTE)
    elseif (button == string.byte('3')) then
      press_vk(VK_VOLUME_UP)
    else
      print('Unknown key:', button)
    end
  end
end)
