
static int diagcharmdm_open(struct inode *inode, struct file *file)
{
	int i = 0;

	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
			current->comm, current->parent->comm, current->tgid);

	if (driver) {
		mutex_lock(&driver->diagcharmdm_mutex);

		for (i = 0; i < driver->num_mdmclients; i++)
			if (driver->mdmclient_map[i].pid == 0)
				break;

		if (i < driver->num_mdmclients) {
			driver->mdmclient_map[i].pid = current->tgid;
			strncpy(driver->mdmclient_map[i].name, current->comm, 20);
			driver->mdmclient_map[i].name[19] = '\0';
		} else {
			mutex_unlock(&driver->diagcharmdm_mutex);
			DIAG_INFO("%s:reach max client count\n", __func__);
			for (i = 0; i < driver->num_clients; i++)
				DIAG_WARNING("%d) %s PID=%d", i, driver->
					mdmclient_map[i].name,
					driver->mdmclient_map[i].pid);
			return -ENOMEM;
		}

		driver->mdmdata_ready[i] |= MSG_MASKS_TYPE;
		driver->mdmdata_ready[i] |= EVENT_MASKS_TYPE;
		driver->mdmdata_ready[i] |= LOG_MASKS_TYPE;

		if (driver->ref_count == 0)
			diagmem_init(driver);
		driver->ref_count++;

		mutex_unlock(&driver->diagcharmdm_mutex);
		return 0;
	}

	return -ENOMEM;

}

static int diagcharmdm_close(struct inode *inode, struct file *file)
{

	int i = 0;

	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
			current->comm, current->parent->comm, current->tgid);

	if (driver) {
#ifdef CONFIG_DIAG_OVER_USB
		
		if (driver->mdm_logging_process_id == current->tgid) {
			driver->logging_mode = USB_MODE;
			diagfwd_connect();
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
			diag_clear_hsic_tbl();
			diagfwd_cancel_hsic();
			diagfwd_connect_bridge(0);
#endif
		}
#endif 

		mutex_lock(&driver->diagcharmdm_mutex);

		driver->ref_count--;
		
		diagmem_exit(driver, POOL_TYPE_COPY);
		diagmem_exit(driver, POOL_TYPE_HDLC);
		diagmem_exit(driver, POOL_TYPE_WRITE_STRUCT);

		for (i = 0; i < driver->num_mdmclients; i++)
			if (driver->mdmclient_map[i].pid == current->tgid) {
				driver->mdmclient_map[i].pid = 0;
				break;
			}

		if (i < driver->num_mdmclients)
			DIAG_INFO("%s:#%d(%d) %s close\n", __func__,
				i, current->tgid, current->comm);
		else
			DIAG_WARNING("%s: nothing close\n", __func__);
		mutex_unlock(&driver->diagcharmdm_mutex);
		return 0;
	}

	return -ENOMEM;
}

static long diagcharmdm_ioctl(struct file *filp,
		unsigned int iocmd, unsigned long ioarg)
{
	int success = -1, i;

	if (iocmd == DIAG_IOCTL_SWITCH_LOGGING) {
		mutex_lock(&driver->diagcharmdm_mutex);
		driver->logging_mode = (int)ioarg;
		driver->mdm_logging_process_id = current->tgid;
		mutex_unlock(&driver->diagcharmdm_mutex);
		if (driver->logging_mode == MEMORY_DEVICE_MODE) {
			DIAG_INFO("diagcharmdm_ioctl enable\n");
			
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
			diagfwd_cancel_hsic();
			diagfwd_connect_bridge(0);
#endif
			driver->qxdm2sd_drop = 0;
		} else if (driver->logging_mode == USB_MODE) {
			DIAG_INFO("diagcharmdm_ioctl disable\n");
			
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
			diag_clear_hsic_tbl();
			diagfwd_cancel_hsic();
			diagfwd_connect_bridge(0);
#endif
			driver->qxdm2sd_drop = 1;
		}
		success = 1;
	} else if (iocmd == DIAG_IOCTL_NONBLOCKING_TIMEOUT) {
		for (i = 0; i < driver->num_mdmclients; i++)
			if (driver->mdmclient_map[i].pid == current->tgid)
				break;
		if (i == -1)
			return -EINVAL;
		mutex_lock(&driver->diagcharmdm_mutex);
		driver->mdmclient_map[i].timeout = (int)ioarg;
		mutex_unlock(&driver->diagcharmdm_mutex);

		success = 1;
	}
	return success;
}

static int diagcharmdm_read(struct file *file, char __user *buf, size_t count,
		loff_t *ppos)
{

	int index = -1, i = 0, ret = 0, timeout = 0;
	int num_data = 0, data_type;

#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
	unsigned long spin_lock_flags;
	struct diag_write_device hsic_buf_tbl[NUM_HSIC_BUF_TBL_ENTRIES];
#endif

	if (diag9k_debug_mask)
		DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
			current->comm, current->parent->comm, current->tgid);

	for (i = 0; i < driver->num_mdmclients; i++)
		if (driver->mdmclient_map[i].pid == current->tgid) {
			index = i;
			timeout = driver->mdmclient_map[i].timeout;
		}

	if (index == -1) {
		DIAG_ERR("%s:%s(parent:%s): tgid=%d "
				"Client PID not found in table\n", __func__,
				current->comm, current->parent->comm, current->tgid);
		for (i = 0; i < driver->num_mdmclients; i++)
			DIAG_ERR("\t#%d: %d\n", i, driver->mdmclient_map[i].pid);
		return -EINVAL;
	}

	if (timeout)
		wait_event_interruptible_timeout(driver->mdmwait_q,
				driver->mdmdata_ready[index], timeout * HZ);
	else
		wait_event_interruptible(driver->mdmwait_q,
				driver->mdmdata_ready[index]);

	mutex_lock(&driver->diagcharmdm_mutex);

	if ((driver->mdmdata_ready[index] & USER_SPACE_DATA_TYPE) && (driver->
				logging_mode == MEMORY_DEVICE_MODE)) {
		
		data_type = driver->data_ready[index] & USER_SPACE_DATA_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		
		ret += 4;

#if defined(CONFIG_DIAGFWD_BRIDGE_CODE)
		spin_lock_irqsave(&driver->hsic_spinlock, spin_lock_flags);
		for (i = 0; i < driver->poolsize_hsic_write; i++) {
			hsic_buf_tbl[i].buf = driver->hsic_buf_tbl[i].buf;
			driver->hsic_buf_tbl[i].buf = 0;
			hsic_buf_tbl[i].length =
				driver->hsic_buf_tbl[i].length;
			driver->hsic_buf_tbl[i].length = 0;
		}
		driver->num_hsic_buf_tbl_entries = 0;
		spin_unlock_irqrestore(&driver->hsic_spinlock,
				spin_lock_flags);

		for (i = 0; i < driver->poolsize_hsic_write; i++) {
			if (hsic_buf_tbl[i].length > 0) {
				if (hsic_buf_tbl[i].buf == NULL)
					goto drop_hsic_mdm_1;
				pr_debug("diag: HSIC copy to user, i: %d, buf: %x, len: %d\n",
						i, (unsigned int)hsic_buf_tbl[i].buf,
						hsic_buf_tbl[i].length);
				num_data++;
				
				if (count < ret+4 || copy_to_user(buf+ret,
					(void *)&(hsic_buf_tbl[i].length), 4)) {
					num_data--;
					pr_info("%s: cannot copy to user: %d/%d\n",
						__func__, ret+4, count);
					goto drop_hsic_mdm_1;
				}
				ret += 4;

				
				if (count < ret+hsic_buf_tbl[i].length ||
					copy_to_user(buf+ret, (void *)hsic_buf_tbl[i].buf,
					hsic_buf_tbl[i].length)) {
					ret -= 4;
					num_data--;
					pr_info("%s: cannot copy to user: %d/%d\n",
						__func__, ret+hsic_buf_tbl[i].length, count);
					goto drop_hsic_mdm_1;
				}
				ret += hsic_buf_tbl[i].length;
drop_hsic_mdm_1:

				
				diagmem_free(driver,
					(unsigned char *)(hsic_buf_tbl[i].buf),
					POOL_TYPE_HSIC);

				
				diagfwd_write_complete_hsic(NULL);
			}
		}
#endif
		
		COPY_USER_SPACE_OR_EXIT(buf+4, num_data, 4);
		ret -= 4;

		driver->mdmdata_ready[index] ^= USER_SPACE_DATA_TYPE;

		if (driver->hsic_ch)
			queue_work(diag_bridge[HSIC].wq, &driver->diag_read_hsic_work);
		goto exit;
	} else if (driver->mdmdata_ready[index] & USER_SPACE_DATA_TYPE) {
		driver->mdmdata_ready[index] ^= USER_SPACE_DATA_TYPE;
	} else if (driver->mdmdata_ready[index] & USERMODE_DIAGFWD) {
		data_type = USERMODE_DIAGFWD_LEGACY;
		driver->mdmdata_ready[index] ^= USERMODE_DIAGFWD;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);

#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
		spin_lock_irqsave(&driver->hsic_spinlock, spin_lock_flags);
		for (i = 0; i < driver->poolsize_hsic_write; i++) {
			hsic_buf_tbl[i].buf = driver->hsic_buf_tbl[i].buf;
			driver->hsic_buf_tbl[i].buf = 0;
			hsic_buf_tbl[i].length =
					driver->hsic_buf_tbl[i].length;
			driver->hsic_buf_tbl[i].length = 0;
		}
		driver->num_hsic_buf_tbl_entries = 0;
		spin_unlock_irqrestore(&driver->hsic_spinlock,
					spin_lock_flags);

		for (i = 0; i < driver->poolsize_hsic_write; i++) {
			if (hsic_buf_tbl[i].length > 0) {
				if (hsic_buf_tbl[i].buf == NULL)
					goto drop_hsic_mdm_2;
				pr_debug("diag: HSIC copy to user, i: %d, buf: %x, len: %d\n",
						i, (unsigned int)hsic_buf_tbl[i].buf,
						hsic_buf_tbl[i].length);
				
				if (count < ret+hsic_buf_tbl[i].length ||
						copy_to_user(buf+ret, (void *)hsic_buf_tbl[i].buf,
							hsic_buf_tbl[i].length)) {
					pr_info("%s: cannot copy to user: %d/%d\n",
							__func__, ret+hsic_buf_tbl[i].length, count);
					goto drop_hsic_mdm_2;
				}
				ret += hsic_buf_tbl[i].length;
drop_hsic_mdm_2:
				
				diagmem_free(driver,
						(unsigned char *)(hsic_buf_tbl[i].buf),
						POOL_TYPE_HSIC);

				
				diagfwd_write_complete_hsic(NULL);
			}
		}
#endif
		if (diag9k_debug_mask)
			pr_info("%s() return %d byte\n", __func__, ret);
		goto exit;
	}

	if (driver->mdmdata_ready[index] & DEINIT_TYPE) {

		driver->mdmdata_ready[index] ^= DEINIT_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & MSG_MASKS_TYPE) {

		driver->mdmdata_ready[index] ^= MSG_MASKS_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & EVENT_MASKS_TYPE) {

		driver->mdmdata_ready[index] ^= EVENT_MASKS_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & LOG_MASKS_TYPE) {

		driver->mdmdata_ready[index] ^= LOG_MASKS_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & PKT_TYPE) {

		driver->mdmdata_ready[index] ^= PKT_TYPE;
		goto exit;
	}
exit:
	if (ret)
		wake_lock_timeout(&driver->wake_lock, HZ / 2);

	mutex_unlock(&driver->diagcharmdm_mutex);

	return ret;
}

static int diagcharmdm_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{

	int err, pkt_type;
	int payload_size;

	if (diag9k_debug_mask)
		DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
				current->comm, current->parent->comm, current->tgid);

#ifdef CONFIG_DIAG_OVER_USB
	if (((driver->logging_mode == USB_MODE) && (!driver->usb_connected)) ||
			(driver->logging_mode == NO_LOGGING_MODE)) {
		
		return -EIO;
	}
#endif 

	
	err = copy_from_user((&pkt_type), buf, 4);
	
	payload_size = count - 4;
	if (pkt_type == USER_SPACE_DATA_TYPE) {
		err = copy_from_user(driver->user_space_mdm_data, buf + 4,
							 payload_size);
		
		if (driver->mask_check) {
			if (!mask_request_validate(driver->user_space_mdm_data)) {
				DIAG_ERR("mask request Invalid ..cannot send to modem \n");
				return -EFAULT;
			}
		}

		pr_debug("diag: user space data %d\n", payload_size);
		DIAGFWD_9K_RAWDATA(driver->user_space_mdm_data, "9K", DIAG_DBG_WRITE);

#ifdef CONFIG_DIAG_SDIO_PIPE
		
		if (driver->sdio_ch) {
			wait_event_interruptible(driver->wait_q,
				 (sdio_write_avail(driver->sdio_ch) >=
					 payload_size));
			if (driver->sdio_ch && (payload_size > 0)) {
				sdio_write(driver->sdio_ch, (void *)
				   (driver->user_space_mdm_data), payload_size);
			}
		}
#endif
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
		
		if (driver->hsic_ch && (payload_size > 0)) {
			
			if (driver->in_busy_hsic_write) {
				driver->in_busy_hsic_write_wait = 1;
				wait_event_interruptible(driver->wait_q,
					(driver->in_busy_hsic_write != 1));
			}
			driver->in_busy_hsic_write = 1;
			driver->in_busy_hsic_read_on_device = 0;
			err = diag_bridge_write(0, driver->user_space_mdm_data,
							 payload_size);
			if (err) {
				pr_err("diag: err sending mask to MDM: %d\n",
									 err);
				if ((-ESHUTDOWN) != err)
					driver->in_busy_hsic_write = 0;
			}
		}
#endif
		if (diag9k_debug_mask)
			pr_info("%s() %d byte\n", __func__, payload_size);
		return count;
	} else if (driver->mdm_logging_process_id == current->tgid) {
		err = copy_from_user(driver->user_space_mdm_data, buf + 4, payload_size);

		pr_debug("diag: user space data %d\n", payload_size);
		DIAGFWD_9K_RAWDATA(driver->user_space_mdm_data, "9K", DIAG_DBG_WRITE);

#ifdef CONFIG_DIAG_SDIO_PIPE
		if (driver->sdio_ch) {
			sdio_write(driver->sdio_ch, driver->user_space_mdm_data, payload_size);
		}
#endif
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
		if (driver->hsic_ch) {
			diag_bridge_write(0, driver->user_space_mdm_data, payload_size);
			queue_work(diag_bridge[HSIC].wq, &driver->diag_read_hsic_work);
		}
#endif
		if (diag9k_debug_mask)
			pr_info("%s() %d byte\n", __func__, payload_size);
		return count;
	}
	return 0;
}

static const struct file_operations diagcharmdmfops = {
	.owner = THIS_MODULE,
	.read = diagcharmdm_read,
	.write = diagcharmdm_write,
	.unlocked_ioctl = diagcharmdm_ioctl,
	.open = diagcharmdm_open,
	.release = diagcharmdm_close
};

static __maybe_unused void diagcharmdm_setup_cdev(dev_t devno)
{
	int err;

	driver->cdev_mdm = cdev_alloc();
	cdev_init(driver->cdev_mdm, &diagcharmdmfops);

	driver->cdev_mdm->owner = THIS_MODULE;
	driver->cdev_mdm->ops = &diagcharmdmfops;

	err = cdev_add(driver->cdev_mdm, devno, 1);

	if (err) {
		DIAG_ERR("diagchar cdev mdm registration failed !\n\n");
		return;
	}

	device_create(driver->diagchar_class, NULL, devno, (void *)driver, "diag_mdm");
}
